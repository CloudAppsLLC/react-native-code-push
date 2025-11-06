// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "miniz/miniz.h"
#include "winrt/Windows.Foundation.h"
#include "winrt/Windows.Storage.h"
#include "winrt/Windows.Storage.Search.h"
#include "winrt/Windows.Storage.Streams.h"

#include <string_view>
#include <stack>
#include <vector>
#include <array>
#include <string>
#include <filesystem>
#include <cassert>

#include "CodePushNativeModule.h"
#include "FileUtils.h"

namespace Microsoft::CodePush::ReactNative
{
    using namespace winrt;
    using namespace Windows::Foundation;
    using namespace Windows::Storage;
    using namespace Windows::Storage::Search;
    using namespace Windows::Storage::Streams;

    /*static*/ IAsyncOperation<StorageFile> FileUtils::CreateFileFromPathAsync(StorageFolder rootFolder, const std::filesystem::path& relativePath)
    {
        auto relPath{ relativePath };

        std::stack<std::string> pathParts;
        pathParts.push(relPath.filename().string());
        while (relPath.has_parent_path())
        {
            relPath = relPath.parent_path();
            pathParts.push(relPath.filename().string());
        }

        while (pathParts.size() > 1)
        {
            auto itemName{ pathParts.top() };
            rootFolder = co_await rootFolder.CreateFolderAsync(to_hstring(itemName), CreationCollisionOption::OpenIfExists);
            pathParts.pop();
        }
        auto fileName{ pathParts.top() };
        auto file{ co_await rootFolder.CreateFileAsync(to_hstring(fileName), CreationCollisionOption::ReplaceExisting) };
        co_return file;
    }

    /*static*/ IAsyncOperation<hstring> FileUtils::FindFilePathAsync(
        const StorageFolder& rootFolder,
        std::wstring_view fileName)
    {
        try
        {
            // Filter to .bundle files (cheap narrowing)
            std::vector<hstring> fileTypeFilter{};
            fileTypeFilter.push_back(L".bundle");
            fileTypeFilter.push_back(L".jsbundle");

            QueryOptions queryOptions{ CommonFileQuery::OrderByName, fileTypeFilter };

            // Search recursively
            queryOptions.FolderDepth(FolderDepth::Deep);

            // Don’t rely on the system indexer for app-local folders
            queryOptions.IndexerOption(IndexerOption::DoNotUseIndexer);

            auto queryResult = rootFolder.CreateFileQueryWithOptions(queryOptions);
            auto files = co_await queryResult.GetFilesAsync();

            if (files.Size() > 0)
            {
                auto result = files.GetAt(0);
                std::wstring_view bundlePath{ result.Path() };

                // Return path relative to rootFolder
                const auto rootPath = rootFolder.Path();
                const size_t prefixLen = rootPath.size();
                hstring relative = prefixLen < bundlePath.size()
                    ? hstring{ bundlePath.substr(prefixLen + 1) }  // skip the trailing '\'
                : hstring{ result.Name() };

                co_return relative;
            }

            co_return L"";
        }
        catch (...)
        {
            throw;
        }
    }

    // Long-path-safe unzip: read ZIP into memory and use miniz from memory.
    /*static*/ IAsyncAction FileUtils::UnzipAsync(const StorageFile& zipFile, const StorageFolder& destination)
    {
        // Read the whole ZIP into memory to avoid Win32 path usage inside miniz.
        auto buffer = co_await FileIO::ReadBufferAsync(zipFile);
        std::vector<uint8_t> zipData(buffer.Length());
        if (!zipData.empty())
        {
            DataReader::FromBuffer(buffer).ReadBytes(array_view<uint8_t>(zipData));
        }

        mz_bool status;
        mz_zip_archive zip_archive;
        mz_zip_zero_struct(&zip_archive);

        // Initialize reader from memory (not from a path) to avoid MAX_PATH.
        status = mz_zip_reader_init_mem(&zip_archive, zipData.data(), zipData.size(), 0);
        assert(status);
        if (!status)
        {
            co_return;
        }

        auto numFiles{ mz_zip_reader_get_num_files(&zip_archive) };

        for (mz_uint i = 0; i < numFiles; i++)
        {
            mz_zip_archive_file_stat file_stat{};
            status = mz_zip_reader_file_stat(&zip_archive, i, &file_stat);
            assert(status);
            if (!status) continue;

            // Skip directories (ZIP dirs typically end with '/')
            if (mz_zip_reader_is_file_a_directory(&zip_archive, i))
            {
                continue;
            }

            // Build relative path using std::filesystem (safe for splitting)
            const char* fname = file_stat.m_filename;
            if (!fname || !*fname) continue;

            std::filesystem::path filePath{ fname }; // ZIP uses forward slashes; fine on Windows
            auto entryFile{ co_await CreateFileFromPathAsync(destination, filePath) };

            // Open WinRT stream and write the decompressed data in chunks
            auto stream{ co_await entryFile.OpenAsync(FileAccessMode::ReadWrite) };
            auto os{ stream.GetOutputStreamAt(0) };
            DataWriter dw{ os };

            const auto arrBufSize = 8 * 1024;
            std::array<uint8_t, arrBufSize> arrBuf;

            mz_zip_reader_extract_iter_state* pState = mz_zip_reader_extract_iter_new(&zip_archive, i, 0);
            // Read in chunks and write to the DataWriter
            for (;;)
            {
                size_t bytesRead = mz_zip_reader_extract_iter_read(pState, static_cast<void*>(arrBuf.data()), arrBuf.size());
                if (bytesRead == 0)
                    break;

                array_view<const uint8_t> view{ arrBuf.data(), arrBuf.data() + bytesRead };
                dw.WriteBytes(view);
            }
            status = mz_zip_reader_extract_iter_free(pState);
            assert(status);

            co_await dw.StoreAsync();
            co_await dw.FlushAsync();

            dw.Close();
            os.Close();
            stream.Close();
        }

        status = mz_zip_reader_end(&zip_archive);
        assert(status);
    }
}
