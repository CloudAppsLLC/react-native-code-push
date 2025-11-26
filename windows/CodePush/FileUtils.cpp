// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h" // MUST be first

#include "miniz/miniz.h"

#include "winrt/Windows.Foundation.h"
#include "winrt/Windows.Storage.h"
#include "winrt/Windows.Storage.Search.h"
#include "winrt/Windows.Storage.Streams.h"
#include "winrt/Windows.Foundation.Collections.h"

#include <array>
#include <cassert>
#include <cwchar>
#include <filesystem>
#include <stack>
#include <string>
#include <string_view>
#include <vector>
#include <Windows.h> // for MultiByteToWideChar UTF-8 -> UTF-16

#include "CodePushNativeModule.h"
#include "CodePushUtils.h"
#include "FileUtils.h"

namespace Microsoft::CodePush::ReactNative
{
    using namespace winrt;
    using namespace Windows::Foundation;
    using namespace Windows::Storage;
    using namespace Windows::Storage::Search;
    using namespace Windows::Storage::Streams;
    using namespace Windows::Foundation::Collections;

    // -------------------- UTF-8 → UTF-16 helper --------------------
    static inline std::wstring Utf8ToWide(std::string const& utf8)
    {
        if (utf8.empty()) return std::wstring{};
        int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed <= 0) return std::wstring{};
        std::wstring out(static_cast<size_t>(needed), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
        return out;
    }

    // -------------------- sanitizers --------------------
    static inline bool IsInvalidChar(wchar_t ch)
    {
        switch (ch)
        {
        case L'<': case L'>': case L':': case L'"':
        case L'/': case L'\\': case L'|': case L'?': case L'*':
            return true;
        default:
            return (ch < 0x20); // control chars not allowed in names
        }
    }

    static inline bool IsReservedDeviceName(const std::wstring& nameNoExtUpper)
    {
        static const std::wstring reserved[] = {
            L"CON", L"PRN", L"AUX", L"NUL",
            L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
            L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
        };
        for (auto const& r : reserved) if (nameNoExtUpper == r) return true;
        return false;
    }

    static inline void TrimTrailingDotsAndSpaces(std::wstring& s)
    {
        while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    }

    static inline void ReplaceInvalidChars(std::wstring& s)
    {
        for (auto& ch : s) if (IsInvalidChar(ch)) ch = L'_';
    }

    // Return false if segment should be skipped entirely
    static inline bool SanitizeSegment(std::wstring& seg)
    {
        if (seg.empty()) return false;

        ReplaceInvalidChars(seg);
        TrimTrailingDotsAndSpaces(seg);

        if (seg.empty() || seg == L"." || seg == L"..") return false;

        // Reserved device names (without extension)
        std::wstring nameNoExt = seg;
        auto dot = nameNoExt.find_last_of(L'.');
        if (dot != std::wstring::npos) nameNoExt = nameNoExt.substr(0, dot);

        std::wstring upper; upper.reserve(nameNoExt.size());
        for (auto c : nameNoExt) upper.push_back(::towupper(c));
        if (IsReservedDeviceName(upper))
        {
            // Prefix with underscore to avoid rejection
            seg.insert(seg.begin(), L'_');
        }

        // Clamp single segment length to be safe (Windows usually ~255)
        constexpr size_t kMaxSegmentLen = 240; // headroom
        if (seg.size() > kMaxSegmentLen)
        {
            seg.resize(kMaxSegmentLen);
            TrimTrailingDotsAndSpaces(seg);
            if (seg.empty()) return false;
        }

        return true;
    }

    // -------------------- path building helpers --------------------
    static std::vector<hstring> SplitPathSegments(hstring const& pathLike)
    {
        std::vector<hstring> parts;
        std::wstring p(pathLike.c_str());
        std::replace(p.begin(), p.end(), L'\\', L'/');
        size_t start = 0;
        for (size_t i = 0; i <= p.size(); ++i)
        {
            if (i == p.size() || p[i] == L'/')
            {
                if (i > start) parts.emplace_back(hstring(p.substr(start, i - start)));
                start = i + 1;
            }
        }
        return parts;
    }

    static IAsyncOperation<StorageFolder> EnsureFolderChainAsync(StorageFolder const& root,
        std::vector<hstring> const& segments)
    {
        StorageFolder cur = root;
        for (auto const& segHs : segments)
        {
            std::wstring segW(segHs.c_str());
            if (!SanitizeSegment(segW)) continue;
            cur = co_await cur.CreateFolderAsync(hstring{ segW }, CreationCollisionOption::OpenIfExists);
        }
        co_return cur;
    }

    // -------------------- FileUtils API --------------------

    /*static*/ IAsyncOperation<StorageFile>
        FileUtils::CreateFileFromPathAsync(StorageFolder rootFolder, const std::filesystem::path& relativePath)
    {
        auto relPath = relativePath;

        // Stack of UTF-8 segments -> we’ll sanitize as UTF-16
        std::stack<std::string> utf8Parts;
        utf8Parts.push(relPath.filename().string());
        while (relPath.has_parent_path())
        {
            relPath = relPath.parent_path();
            utf8Parts.push(relPath.filename().string());
        }

        // Walk folders (all but the last)
        while (utf8Parts.size() > 1)
        {
            std::wstring segW = Utf8ToWide(utf8Parts.top());
            utf8Parts.pop();
            if (!SanitizeSegment(segW)) continue;

            rootFolder = co_await rootFolder.CreateFolderAsync(
                hstring{ segW },
                CreationCollisionOption::OpenIfExists);
        }

        // Create file
        std::wstring fileSegW = Utf8ToWide(utf8Parts.top());
        if (!SanitizeSegment(fileSegW))
        {
            throw hresult_error(E_INVALIDARG, L"ZIP entry has invalid file name after sanitization.");
        }

        StorageFile file = co_await rootFolder.CreateFileAsync(
            hstring{ fileSegW },
            CreationCollisionOption::ReplaceExisting);

        co_return file;
    }

    /*static*/ IAsyncOperation<hstring>
        FileUtils::FindFilePathAsync(const StorageFolder& rootFolder, std::wstring_view expectedFileName)
    {
        // 1) If caller gave an exact expected file name (e.g., "index.windows.bundle"), try to find it exactly
        if (!expectedFileName.empty())
        {
            // Deep, no indexer
            QueryOptions qo;
            qo.FolderDepth(FolderDepth::Deep);
            qo.IndexerOption(IndexerOption::DoNotUseIndexer);

            auto q = rootFolder.CreateFileQueryWithOptions(qo);
            auto files = co_await q.GetFilesAsync();

            for (auto const& f : files)
            {
                if (_wcsicmp(f.Name().c_str(), expectedFileName.data()) == 0)
                {
                    // Return relative path to rootFolder
                    std::wstring full = f.Path().c_str();
                    std::wstring root = rootFolder.Path().c_str();
                    if (full.size() > root.size() + 1)
                        co_return hstring{ std::wstring{ full.begin() + root.size() + 1, full.end() } };
                    co_return f.Name();
                }
            }
        }

        // If expectedFileName contains subfolders, navigate through them
        if (!expectedFileName.empty())
        {
            std::wstring expectedStr{ expectedFileName.data() };
            std::replace(expectedStr.begin(), expectedStr.end(), L'/', L'\\');
            std::vector<std::wstring> segments;
            size_t start = 0;
            for (size_t i = 0; i <= expectedStr.size(); ++i)
            {
                if (i == expectedStr.size() || expectedStr[i] == L'\\')
                {
                    if (i > start)
                    {
                        segments.push_back(expectedStr.substr(start, i - start));
                    }
                    start = i + 1;
                }
            }
            if (!segments.empty())
            {
                StorageFolder currentFolder = rootFolder;
                for (size_t i = 0; i < segments.size() - 1; ++i)
                {
                    auto item = co_await currentFolder.TryGetItemAsync(hstring{ segments[i] });
                    currentFolder = item.try_as<StorageFolder>();
                    if (!currentFolder) co_return L"";
                }
                auto fileItem = co_await currentFolder.TryGetItemAsync(hstring{ segments.back() });
                if (fileItem)
                {
                    co_return hstring{ segments.back() };
                }
            }
        }

        // 2) Fallback to first *.bundle / *.jsbundle
        auto types = winrt::single_threaded_vector<hstring>({ L".bundle", L".jsbundle" });
        QueryOptions q2{ CommonFileQuery::OrderByName, types };
        q2.FolderDepth(FolderDepth::Deep);
        q2.IndexerOption(IndexerOption::DoNotUseIndexer);

        auto q = rootFolder.CreateFileQueryWithOptions(q2);
        auto files2 = co_await q.GetFilesAsync();
        if (files2.Size() == 0) co_return L"";

        auto f = files2.GetAt(0);
        std::wstring full = f.Path().c_str();
        std::wstring root = rootFolder.Path().c_str();
        if (full.size() > root.size() + 1)
            co_return hstring{ std::wstring{ full.begin() + root.size() + 1, full.end() } };
        co_return f.Name();
    }

    // Long-path safe unzip (from memory) + robust name sanitization
    /*static*/ IAsyncAction
        FileUtils::UnzipAsync(const StorageFile& zipFile, const StorageFolder& destination)
    {
        // Load whole ZIP safely
        IBuffer ibuf = co_await FileIO::ReadBufferAsync(zipFile);
        const uint32_t zipLen = ibuf ? ibuf.Length() : 0;
        CodePushUtils::Log(L"[Unzip] ZIP buffer length: " + to_hstring(zipLen));
        if (zipLen == 0) {
            CodePushUtils::Log(L"[Unzip] ZIP buffer is empty.");
            co_return;
        }

        std::vector<uint8_t> zipData(zipLen);
        {
            auto dr = DataReader::FromBuffer(ibuf);
            dr.ReadBytes(winrt::array_view<uint8_t>(zipData));
        }

        mz_zip_archive za{};
        mz_zip_zero_struct(&za);

        if (!mz_zip_reader_init_mem(&za, zipData.data(), zipData.size(), 0)) {
            CodePushUtils::Log(L"[Unzip] Failed to init ZIP reader from memory.");
            co_return;
        }

        const mz_uint numFiles = mz_zip_reader_get_num_files(&za);
        CodePushUtils::Log(L"[Unzip] Number of files in ZIP: " + to_hstring(numFiles));

        // Safety rails (defense-in-depth for Release)
        constexpr size_t kMaxEntryBytes = size_t(200) * 1024 * 1024; // 200 MB per file
        constexpr size_t kMaxTotalBytes = size_t(1024) * 1024 * 1024; // 1 GB per zip
        size_t totalOut = 0;

        for (mz_uint i = 0; i < numFiles; ++i)
        {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&za, i, &st)) continue;

            // Directories are implicitly created; skip them
            if (mz_zip_reader_is_file_a_directory(&za, i)) continue;

            const char* cname = st.m_filename;
            if (!cname || !*cname) continue;

            // Normalize/sanitize entry name early
            std::wstring wname = Utf8ToWide(std::string{ cname });
            if (wname.empty()) continue;

            // Skip absolute/odd roots like "/foo" or ".",".."
            if (wname[0] == L'/' || wname == L"." || wname == L"..") continue;

            // Convert to std::filesystem path (POSIX separators OK)
            std::filesystem::path rel{ cname };

            CodePushUtils::Log(L"[Unzip] Extracting: " + hstring{ wname } + L" size=" + to_hstring(st.m_uncomp_size));

            // Size rails
            if (st.m_uncomp_size > kMaxEntryBytes) {
                CodePushUtils::Log(L"[Unzip] Skipping oversized entry: " + hstring{ wname });
                continue;
            }
            if (totalOut + st.m_uncomp_size > kMaxTotalBytes) {
                CodePushUtils::Log(L"[Unzip] Aborting unzip: total size limit exceeded.");
                break;
            }

            // Extract whole file to heap
            size_t outSize = 0;
            void* heapData = mz_zip_reader_extract_to_heap(&za, i, &outSize, 0);
            if (!heapData) {
                CodePushUtils::Log(L"[Unzip] Failed to extract: " + hstring{ wname });
                continue;
            }

            try
            {
                CodePushUtils::Log(L"[Unzip] Writing file: " + hstring{ wname });
                // Create the destination file (sanitization happens inside)
                StorageFile outFile = co_await CreateFileFromPathAsync(destination, rel);

                // Write in one go (fewer async transitions, less chance to corrupt state)
                auto rw = co_await outFile.OpenAsync(FileAccessMode::ReadWrite);
                auto out = rw.GetOutputStreamAt(0);
                DataWriter dw{ out };

                dw.WriteBytes(winrt::array_view<const uint8_t>(
                    static_cast<uint8_t const*>(heapData),
                    static_cast<uint8_t const*>(heapData) + outSize));

                co_await dw.StoreAsync();
                co_await dw.FlushAsync();
                dw.DetachStream();
                out.Close();
                rw.Close();

                totalOut += outSize;
                CodePushUtils::Log(L"[Unzip] File written: " + outFile.Path());
            }
            catch (winrt::hresult_error const& ex)
            {
                wchar_t hrHex[11]{};
                _snwprintf_s(hrHex, _countof(hrHex), _TRUNCATE, L"0x%08X", static_cast<uint32_t>(ex.code().value));
                CodePushUtils::Log(L"[Unzip] Write failed: " + hstring{ wname } + L" hr=" + hstring{ hrHex });
            }

            // Free heap allocation from miniz
            mz_free(heapData);
        }

        mz_zip_reader_end(&za);
        CodePushUtils::Log(L"[Unzip] Extraction complete. Total bytes: " + to_hstring(totalOut));
        co_return;
    }

}
