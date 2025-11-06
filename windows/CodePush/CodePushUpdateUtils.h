#pragma once

#include "winrt/Windows.Data.Json.h"
#include "winrt/Windows.Foundation.h"
#include "winrt/Windows.Storage.h"

#include <string_view>

namespace Microsoft::CodePush::ReactNative
{
    struct CodePushUpdateUtils
    {
        static constexpr std::wstring_view AssetsFolderName = L"assets";
        static constexpr std::wstring_view BinaryHashKey = L"CodePushBinaryHash";
        static constexpr std::wstring_view ManifestFolderPrefix = L"CodePush";
        static constexpr std::wstring_view BundleJWTFile = L".codepushrelease";

        static winrt::Windows::Foundation::IAsyncAction CopyEntriesInFolderAsync(
            winrt::Windows::Storage::StorageFolder const& sourceRoot,
            winrt::Windows::Storage::StorageFolder const& destRoot);

        static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> ModifiedDateStringOfFileAsync(
            winrt::Windows::Storage::StorageFile const& file);

        static winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Storage::StorageFile> GetSignatureFileAsync(
            winrt::Windows::Storage::StorageFolder const& rootFolder);

    private:
        static constexpr std::wstring_view IgnoreMacOSX = L"__MACOSX/";
        static constexpr std::wstring_view IgnoreDSStore = L".DS_Store";
        static constexpr std::wstring_view IgnoreCodePushMetadata = L".codepushrelease";
    };
}
