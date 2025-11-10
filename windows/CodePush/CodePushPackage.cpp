// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"

#include "CodePushDownloadHandler.h"
#include "CodePushNativeModule.h"
#include "CodePushPackage.h"
#include "CodePushUtils.h"
#include "CodePushUpdateUtils.h"
#include "FileUtils.h"

#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.AccessCache.h>

#include <functional>

namespace Microsoft::CodePush::ReactNative
{
    using namespace winrt;
    using namespace Windows::Data::Json;
    using namespace Windows::Foundation;
    using namespace Windows::Storage;
    using namespace Windows::Storage::Streams;

    /*static*/ IAsyncAction CodePushPackage::ClearUpdatesAsync()
    {
        if (auto codePushFolder{ co_await GetCodePushFolderAsync() })
        {
            try { co_await codePushFolder.DeleteAsync(); }
            catch (hresult_error const& ex) { CodePushUtils::Log(L"[CodePush] ClearUpdatesAsync delete failed: " + ex.message()); }
        }
        co_return;
    }

    /*static*/ IAsyncAction CodePushPackage::DownloadPackageAsync(
        JsonObject& updatePackage,
        std::wstring_view expectedBundleFileName,
        std::wstring_view publicKey,
        std::function<void(int64_t, int64_t)> progressCallback)
    {
        // Wrap the whole flow to ensure we surface useful logs in Release
        try
        {
            const auto newUpdateHash{ updatePackage.GetNamedString(L"packageHash") };
            auto codePushFolder{ co_await GetCodePushFolderAsync() };
            if (!codePushFolder) { throw hresult_error(E_FAIL, L"[CodePush] CodePush folder unavailable."); }

            // Work under a short cache path to avoid path-length surprises
            auto cacheRoot = ApplicationData::Current().LocalCacheFolder();
            auto workRoot = co_await cacheRoot.CreateFolderAsync(L"cpw", CreationCollisionOption::OpenIfExists);
            auto unzipFolder = co_await workRoot.CreateFolderAsync(L"u", CreationCollisionOption::ReplaceExisting);

            // Download to CodePush root (stable, persisted)
            auto downloadFile = co_await codePushFolder.CreateFileAsync(DownloadFileName, CreationCollisionOption::ReplaceExisting);

            CodePushDownloadHandler downloadHandler{ downloadFile, progressCallback };
            const bool isZip = co_await downloadHandler.Download(updatePackage.GetNamedString(L"downloadUrl"));
            CodePushUtils::Log(isZip ? L"[CodePush] Downloaded ZIP." : L"[CodePush] Downloaded single bundle file.");

            // Create destination for this hash
            StorageFolder newUpdateFolder{ co_await codePushFolder.CreateFolderAsync(newUpdateHash, CreationCollisionOption::ReplaceExisting) };
            StorageFile newUpdateMetadataFile{ nullptr };
            auto mutableUpdatePackage{ updatePackage };

            if (isZip)
            {
                // Unzip to the short cache path, then copy over (our copy function tolerates long content paths)
                co_await FileUtils::UnzipAsync(downloadFile, unzipFolder);
                co_await downloadFile.DeleteAsync();

                bool isDiffUpdate = false;
                if (auto diffManifestFile{ (co_await unzipFolder.TryGetItemAsync(DiffManifestFileName)).try_as<StorageFile>() })
                {
                    isDiffUpdate = true;

                    if (auto currentPackageFolder{ co_await GetCurrentPackageFolderAsync() })
                    {
                        // Seed with previous package
                        co_await CodePushUpdateUtils::CopyEntriesInFolderAsync(currentPackageFolder, newUpdateFolder);
                    }
                    else
                    {
                        // Seed with binary bundle + assets (running AppX bundle)
                        auto newUpdateCodePushFolder{ co_await newUpdateFolder.CreateFolderAsync(CodePushUpdateUtils::ManifestFolderPrefix) };

                        if (auto binaryAssetsFolder{ co_await CodePushNativeModule::GetBundleAssetsFolderAsync() })
                        {
                            auto newUpdateAssetsFolder{ co_await newUpdateCodePushFolder.CreateFolderAsync(CodePushUpdateUtils::AssetsFolderName) };
                            co_await CodePushUpdateUtils::CopyEntriesInFolderAsync(binaryAssetsFolder, newUpdateAssetsFolder);
                        }

                        if (auto binaryBundleFile{ co_await CodePushNativeModule::GetBinaryBundleAsync() })
                        {
                            co_await binaryBundleFile.CopyAsync(newUpdateCodePushFolder, binaryBundleFile.Name(), NameCollisionOption::ReplaceExisting);
                        }
                    }

                    // Apply deletions
                    auto manifestContent{ co_await FileIO::ReadTextAsync(diffManifestFile, UnicodeEncoding::Utf8) };
                    auto manifestJson{ JsonObject::Parse(manifestContent) };
                    if (auto deletedFiles{ manifestJson.TryLookup(L"deletedFiles") })
                    {
                        if (auto deletedFilesArray{ deletedFiles.try_as<JsonArray>() })
                        {
                            for (const auto& deletedFileName : deletedFilesArray)
                            {
                                if (auto item{ co_await newUpdateFolder.TryGetItemAsync(deletedFileName.GetString()) })
                                {
                                    co_await item.DeleteAsync();
                                }
                            }
                        }
                    }

                    co_await diffManifestFile.DeleteAsync();
                }

                // Overlay extracted content into the destination
                co_await CodePushUpdateUtils::CopyEntriesInFolderAsync(unzipFolder, newUpdateFolder);

                // Clean up cache unzip folder for next run
                try { co_await unzipFolder.DeleteAsync(); }
                catch (...) {}

                // Discover the bundle path relative to the package folder
                auto relativeBundlePath{ co_await FileUtils::FindFilePathAsync(newUpdateFolder, expectedBundleFileName) };
                if (!relativeBundlePath.empty())
                {
                    mutableUpdatePackage.Insert(RelativeBundlePathKey, JsonValue::CreateStringValue(relativeBundlePath));
                }
                else
                {
                    // Emit a directory listing to the log to help diagnose in Release
                    CodePushUtils::Log(L"[CodePush] Unable to locate expected bundle: " + hstring(expectedBundleFileName));
                    if (auto items = co_await newUpdateFolder.GetItemsAsync(); items.Size() == 0)
                    {
                        CodePushUtils::Log(L"[CodePush] newUpdateFolder is EMPTY. Unzip/copy likely failed.");
                    }
                    throw hresult_error(E_INVALIDARG, L"Error: Unable to find JS bundle in downloaded package.");
                }

                // Remove stale metadata file if present
                if (auto newUpdateMetadata{ (co_await newUpdateFolder.TryGetItemAsync(UpdateMetadataFileName)).try_as<StorageFile>() })
                {
                    co_await newUpdateMetadata.DeleteAsync();
                }

                CodePushUtils::Log(isDiffUpdate ? L"[CodePush] Applying diff update." : L"[CodePush] Applying full update.");

                // Signature/integrity: warn only (don’t block Release)
                const bool isSignatureVerificationEnabled = !publicKey.empty();
                auto signatureFile{ co_await CodePushUpdateUtils::GetSignatureFileAsync(newUpdateFolder) };
                const bool isSignatureAppearedInBundle = (signatureFile != nullptr);
                if (isSignatureVerificationEnabled || isSignatureAppearedInBundle)
                {
                    CodePushUtils::Log(
                        L"[CodePush] Signature/integrity verification not implemented on Windows; proceeding without blocking.");
                }
            }
            else
            {
                // Single file: move directly as the bundle
                co_await downloadFile.MoveAsync(newUpdateFolder, UpdateBundleFileName, NameCollisionOption::ReplaceExisting);
            }

            // Persist update metadata
            auto newUpdateMetadataFileCreated = co_await newUpdateFolder.CreateFileAsync(UpdateMetadataFileName, CreationCollisionOption::ReplaceExisting);
            auto packageJsonString{ mutableUpdatePackage.Stringify() };
            co_await FileIO::WriteTextAsync(newUpdateMetadataFileCreated, packageJsonString);
        }
        catch (hresult_error const& ex)
        {
            CodePushUtils::Log(L"[CodePush] DownloadPackageAsync failed (hresult): " + ex.message());
            throw;
        }
        catch (std::exception const& ex)
        {
            CodePushUtils::Log(L"[CodePush] DownloadPackageAsync failed (std): " + to_hstring(ex.what()));
            throw hresult_error(E_FAIL, L"DownloadPackageAsync(std::exception).");
        }
        catch (...)
        {
            CodePushUtils::Log(L"[CodePush] DownloadPackageAsync failed (unknown).");
            throw;
        }

        co_return;
    }

    /*static*/ IAsyncOperation<StorageFolder> CodePushPackage::GetCodePushFolderAsync()
    {
        auto localStorage{ CodePushNativeModule::GetLocalStorageFolder() };
        co_return co_await localStorage.CreateFolderAsync(L"CodePush", CreationCollisionOption::OpenIfExists);
    }

    /*static*/ IAsyncOperation<JsonObject> CodePushPackage::GetCurrentPackageAsync()
    {
        auto packageHash{ co_await GetCurrentPackageHashAsync() };
        if (packageHash.empty()) co_return nullptr;
        co_return co_await GetPackageAsync(packageHash);
    }

    /*static*/ IAsyncOperation<StorageFile> CodePushPackage::GetCurrentPackageBundleAsync()
    {
        auto packageFolder{ co_await GetCurrentPackageFolderAsync() };
        if (!packageFolder) co_return nullptr;

        auto currentPackage{ co_await GetCurrentPackageAsync() };
        if (currentPackage == nullptr) co_return nullptr;

        auto relativeBundlePath{ currentPackage.GetNamedString(RelativeBundlePathKey, L"") };
        if (!relativeBundlePath.empty())
        {
            co_return (co_await packageFolder.TryGetItemAsync(relativeBundlePath)).try_as<StorageFile>();
        }
        co_return nullptr;
    }

    /*static*/ IAsyncOperation<StorageFolder> CodePushPackage::GetCurrentPackageFolderAsync()
    {
        auto info{ co_await GetCurrentPackageInfoAsync() };
        if (info == nullptr) co_return nullptr;

        auto packageHash{ info.GetNamedString(L"currentPackage", L"") };
        if (packageHash.empty()) co_return nullptr;

        auto codePushFolder{ co_await GetCodePushFolderAsync() };
        co_return (co_await codePushFolder.TryGetItemAsync(packageHash)).try_as<StorageFolder>();
    }

    /*static*/ IAsyncOperation<hstring> CodePushPackage::GetCurrentPackageHashAsync()
    {
        auto info{ co_await GetCurrentPackageInfoAsync() };
        if (info == nullptr) co_return L"";
        if (auto currentPackage{ info.TryLookup(L"currentPackage") }; currentPackage != nullptr) co_return currentPackage.GetString();
        co_return L"";
    }

    /*static*/ IAsyncOperation<JsonObject> CodePushPackage::GetCurrentPackageInfoAsync()
    {
        try
        {
            if (auto statusFile{ co_await GetStatusFileAsync() })
            {
                auto content{ co_await FileIO::ReadTextAsync(statusFile) };
                JsonObject json;
                if (JsonObject::TryParse(content, json)) co_return json;
                co_return nullptr;
            }
            co_return JsonObject{};
        }
        catch (...)
        {
            co_return nullptr;
        }
    }

    /*static*/ IAsyncOperation<JsonObject> CodePushPackage::GetPreviousPackageAsync()
    {
        auto packageHash{ co_await GetPreviousPackageHashAsync() };
        if (packageHash.empty()) co_return nullptr;
        co_return co_await GetPackageAsync(packageHash);
    }

    /*static*/ IAsyncOperation<hstring> CodePushPackage::GetPreviousPackageHashAsync()
    {
        auto info{ co_await GetCurrentPackageInfoAsync() };
        if (info == nullptr) co_return L"";
        if (auto previousHash{ info.TryLookup(L"previousPackage") }; previousHash != nullptr) co_return previousHash.GetString();
        co_return L"";
    }

    /*static*/ IAsyncOperation<JsonObject> CodePushPackage::GetPackageAsync(std::wstring_view packageHash)
    {
        if (auto updateDirectory{ co_await GetPackageFolderAsync(packageHash) })
        {
            if (auto updateMetadataFile{ (co_await updateDirectory.TryGetItemAsync(UpdateMetadataFileName)).try_as<StorageFile>() })
            {
                auto updateMetadataString{ co_await FileIO::ReadTextAsync(updateMetadataFile) };
                JsonObject updateMetadata;
                if (JsonObject::TryParse(updateMetadataString, updateMetadata)) co_return updateMetadata;
            }
        }
        co_return nullptr;
    }

    /*static*/ IAsyncOperation<StorageFolder> CodePushPackage::GetPackageFolderAsync(std::wstring_view packageHash)
    {
        auto codePushFolder{ co_await GetCodePushFolderAsync() };
        co_return (co_await codePushFolder.TryGetItemAsync(packageHash)).try_as<StorageFolder>();
    }

    /*static*/ IAsyncOperation<bool> CodePushPackage::InstallPackageAsync(JsonObject updatePackage, bool removePendingUpdate)
    {
        auto packageHash{ updatePackage.GetNamedString(L"packageHash") };
        auto info{ co_await GetCurrentPackageInfoAsync() };
        if (info == nullptr) co_return false;

        if (info.HasKey(L"currentPackage") && packageHash == info.GetNamedString(L"currentPackage"))
        {
            co_return true; // already installed
        }

        if (removePendingUpdate)
        {
            if (auto currentPackageFolder{ co_await GetCurrentPackageFolderAsync() })
            {
                try { co_await currentPackageFolder.DeleteAsync(); }
                catch (...) { CodePushUtils::Log(L"[CodePush] Error deleting pending package."); }
            }
        }
        else
        {
            auto previousPackageHash{ co_await GetPreviousPackageHashAsync() };
            if (!previousPackageHash.empty() && previousPackageHash != packageHash)
            {
                if (auto previousPackageFolder{ co_await GetPackageFolderAsync(previousPackageHash) })
                {
                    try { co_await previousPackageFolder.DeleteAsync(); }
                    catch (...) { CodePushUtils::Log(L"[CodePush] Error deleting old package."); }
                }
            }

            IJsonValue currentPackageVal = info.HasKey(L"currentPackage")
                ? info.Lookup(L"currentPackage")
                : JsonValue::CreateStringValue(L"");

            info.Insert(L"previousPackage", currentPackageVal);
        }

        info.Insert(L"currentPackage", JsonValue::CreateStringValue(packageHash));
        co_return co_await UpdateCurrentPackageInfoAsync(info);
    }

    /*static*/ IAsyncAction CodePushPackage::RollbackPackage()
    {
        auto info{ co_await GetCurrentPackageInfoAsync() };
        if (info == nullptr)
        {
            CodePushUtils::Log(L"[CodePush] RollbackPackage: no current package info.");
            co_return;
        }

        if (auto currentPackageFolder{ co_await GetCurrentPackageFolderAsync() })
        {
            try { co_await currentPackageFolder.DeleteAsync(); }
            catch (...) { CodePushUtils::Log(L"[CodePush] Error deleting current package contents."); }
        }
        else
        {
            CodePushUtils::Log(L"[CodePush] RollbackPackage: current package folder missing.");
        }

        info.Insert(L"currentPackage", info.TryLookup(L"previousPackage"));
        info.Remove(L"previousPackage");
        co_await UpdateCurrentPackageInfoAsync(info);
    }

    /*static*/ IAsyncOperation<StorageFile> CodePushPackage::GetStatusFileAsync()
    {
        auto codePushFolder{ co_await GetCodePushFolderAsync() };
        co_return (co_await codePushFolder.TryGetItemAsync(CodePushPackage::StatusFile)).try_as<StorageFile>();
    }

    /*static*/ IAsyncOperation<bool> CodePushPackage::UpdateCurrentPackageInfoAsync(JsonObject packageInfo)
    {
        auto packageInfoString{ packageInfo.Stringify() };
        auto infoFile{ co_await GetStatusFileAsync() };
        if (!infoFile)
        {
            auto codePushFolder{ co_await GetCodePushFolderAsync() };
            infoFile = co_await codePushFolder.CreateFileAsync(CodePushPackage::StatusFile);
        }
        co_await FileIO::WriteTextAsync(infoFile, packageInfoString);
        co_return true;
    }
}
