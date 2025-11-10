// Copyright (c) Microsoft.
// Licensed under the MIT License.

#include "pch.h" // MUST be first
#include "CodePushUpdateUtils.h"
#include "CodePushUtils.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Search.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Globalization.DateTimeFormatting.h>
#include <winrt/Windows.Storage.FileProperties.h>


#include <algorithm>
#include <string>
#include <vector>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::Search;
using namespace Windows::Storage::Streams;
using namespace Windows::Globalization::DateTimeFormatting;

namespace Microsoft::CodePush::ReactNative
{
	// ------------ helpers -----------------------------------------------------

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

	static IAsyncOperation<StorageFolder> EnsureFolderChainAsync(
		StorageFolder const& root,
		std::vector<hstring> const& segments)
	{
		StorageFolder cur = root;
		for (auto const& seg : segments)
		{
			if (seg.empty()) continue;
			if (seg == L"." || seg == L"..") co_return cur;
			cur = co_await cur.CreateFolderAsync(seg, CreationCollisionOption::OpenIfExists);
		}
		co_return cur;
	}

	static IAsyncAction CopyFileEntryAsync(
		StorageFile const& src,
		StorageFolder const& destRoot,
		hstring const& relPath)
	{
		auto segments = SplitPathSegments(relPath);
		if (segments.empty()) co_return;

		hstring fileName = segments.back();
		segments.pop_back();
		StorageFolder parent = co_await EnsureFolderChainAsync(destRoot, segments);

		StorageFile out = co_await parent.CreateFileAsync(fileName, CreationCollisionOption::ReplaceExisting);

		auto inStream = co_await src.OpenReadAsync();
		auto outStream = co_await out.OpenAsync(FileAccessMode::ReadWrite);

		Buffer buf{ 64 * 1024 };
		DataReader reader{ inStream };
		DataWriter writer{ outStream };

		while (inStream.Position() < inStream.Size())
		{
			buf.Length(0);
			co_await reader.LoadAsync(buf.Capacity());
			const auto remaining = reader.UnconsumedBufferLength();
			if (remaining == 0) break;

			auto chunk = reader.ReadBuffer(remaining);
			writer.WriteBuffer(chunk);
			co_await writer.StoreAsync();
		}

		co_await writer.FlushAsync();
		writer.DetachStream();
		inStream.Close();
		outStream.Close();
		co_return;
	}

	// If sourceRoot has a single child folder named "CodePush", return that folder (avoid extra level)
	static IAsyncOperation<StorageFolder> MaybeStripTopCodePushAsync(StorageFolder const& sourceRoot)
	{
		auto items = co_await sourceRoot.GetItemsAsync();
		if (items.Size() == 1)
		{
			if (auto onlyFolder = items.GetAt(0).try_as<StorageFolder>())
			{
				if (_wcsicmp(onlyFolder.Name().c_str(), L"CodePush") == 0)
				{
					co_return onlyFolder;
				}
			}
		}
		co_return sourceRoot;
	}

	// ------------ public ------------------------------------------------------

	// Matches CodePushUpdateUtils.h exactly
	IAsyncAction CodePushUpdateUtils::CopyEntriesInFolderAsync(
		Windows::Storage::StorageFolder const& sourceRoot,
		Windows::Storage::StorageFolder const& destRoot)
	{
		StorageFolder copyRoot = co_await MaybeStripTopCodePushAsync(sourceRoot);

		QueryOptions qo; // default ctor
		qo.FolderDepth(FolderDepth::Deep);
		qo.IndexerOption(IndexerOption::DoNotUseIndexer);

		auto query = copyRoot.CreateFileQueryWithOptions(qo);
		auto files = co_await query.GetFilesAsync();

		const std::wstring rootPathW = copyRoot.Path().c_str();
		const size_t rootLen = rootPathW.size();

		for (auto const& f : files)
		{
			try
			{
				const std::wstring fullW = f.Path().c_str();
				hstring rel;
				if (fullW.size() > rootLen + 1)
				{
					// Construct hstring directly from wchar_t* at substring start
					rel = hstring{ fullW.c_str() + (rootLen + 1) };
				}
				else
				{
					rel = f.Name();
				}

				co_await CopyFileEntryAsync(f, destRoot, rel);
			}
			catch (winrt::hresult_error const& ex)
			{
				const auto hr = static_cast<uint32_t>(ex.code().value);
				wchar_t hrHex[11]{};
				_snwprintf_s(hrHex, _countof(hrHex), _TRUNCATE, L"0x%08X", hr);

				CodePushUtils::Log(
					L"[CopyEntriesInFolderAsync] Failed: " + f.Path() + L" hr=" + winrt::hstring{ hrHex });
			}
		}

		co_return;
	}

	// NEW: definition to satisfy the header (returns formatted DateModified)
	winrt::Windows::Foundation::IAsyncOperation<winrt::hstring>
		CodePushUpdateUtils::ModifiedDateStringOfFileAsync(winrt::Windows::Storage::StorageFile const& file)
	{
		using winrt::Windows::Storage::FileProperties::BasicProperties;

		if (!file) co_return winrt::hstring{};  // null safety

		BasicProperties props = co_await file.GetBasicPropertiesAsync();

		// Windows::Foundation::DateTime .time_since_epoch() is 100-ns ticks since 1601-01-01 (UTC)
		const auto ticks = props.DateModified().time_since_epoch().count();

		// Return as string (stable, culture-agnostic)
		co_return winrt::to_hstring(ticks);
	}


	// NEW: definition to satisfy the header (finds .codepushrelease anywhere under root)
	winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::StorageFile>
		CodePushUpdateUtils::GetSignatureFileAsync(winrt::Windows::Storage::StorageFolder const& rootFolder)
	{
		// Quick check on the root
		if (auto direct = (co_await rootFolder.TryGetItemAsync(winrt::hstring{ L".codepushrelease" })).try_as<StorageFile>())
		{
			co_return direct;
		}

		// Deep search without indexer
		QueryOptions qo;
		qo.FolderDepth(FolderDepth::Deep);
		qo.IndexerOption(IndexerOption::DoNotUseIndexer);

		auto q = rootFolder.CreateFileQueryWithOptions(qo);
		auto files = co_await q.GetFilesAsync();
		for (auto const& f : files)
		{
			if (_wcsicmp(f.Name().c_str(), L".codepushrelease") == 0)
			{
				co_return f;
			}
		}
		co_return nullptr;
	}
}
