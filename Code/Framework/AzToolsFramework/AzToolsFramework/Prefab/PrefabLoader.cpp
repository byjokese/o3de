/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */


#include <AzToolsFramework/Prefab/PrefabLoader.h>

#include <AzCore/Component/Entity.h>
#include <AzCore/IO/Path/Path.h>
#include <AzCore/Settings/SettingsRegistryMergeUtils.h>
#include <AzCore/StringFunc/StringFunc.h>

#include <AzFramework/Asset/AssetSystemBus.h>
#include <AzFramework/FileFunc/FileFunc.h>
#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/API/ToolsApplicationAPI.h>
#include <AzToolsFramework/Prefab/PrefabDomUtils.h>
#include <AzToolsFramework/Prefab/PrefabSystemComponentInterface.h>

namespace AzToolsFramework
{
    namespace Prefab
    {
        void PrefabLoader::RegisterPrefabLoaderInterface()
        {
            m_prefabSystemComponentInterface = AZ::Interface<PrefabSystemComponentInterface>::Get();
            AZ_Assert(
                m_prefabSystemComponentInterface != nullptr,
                "Prefab System Component Interface could not be found. "
                "It is a requirement for the PrefabLoader class. "
                "Check that it is being correctly initialized.");

            auto settingsRegistry = AZ::SettingsRegistry::Get();
            AZ_Assert(settingsRegistry, "Settings registry is not set");
            
            [[maybe_unused]] bool result =
                settingsRegistry->Get(m_projectPathWithOsSeparator.Native(), AZ::SettingsRegistryMergeUtils::FilePathKey_ProjectPath);
            AZ_Warning("Prefab", result, "Couldn't retrieve project root path");
            m_projectPathWithSlashSeparator = AZ::IO::Path(m_projectPathWithOsSeparator.Native(), '/').MakePreferred();

            AZ::Interface<PrefabLoaderInterface>::Register(this);
        }

        void PrefabLoader::UnregisterPrefabLoaderInterface()
        {
            AZ::Interface<PrefabLoaderInterface>::Unregister(this);
        }

        TemplateId PrefabLoader::LoadTemplateFromFile(AZ::IO::PathView filePath)
        {
            AZStd::unordered_set<AZ::IO::Path> progressedFilePathsSet;
            TemplateId newTemplateId = LoadTemplateFromFile(filePath, progressedFilePathsSet);
            return newTemplateId;
        }

        TemplateId PrefabLoader::LoadTemplateFromFile(AZ::IO::PathView filePath, AZStd::unordered_set<AZ::IO::Path>& progressedFilePathsSet)
        {
            if (!IsValidPrefabPath(filePath))
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadTemplateFromFile - "
                    "Invalid file path: '%.*s'.",
                    AZ_STRING_ARG(filePath.Native())
                );
                return InvalidTemplateId;
            }

            auto readResult = AZ::Utils::ReadFile(GetFullPath(filePath).Native(), AZStd::numeric_limits<size_t>::max());
            if (!readResult.IsSuccess())
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadTemplate - Failed to load Prefab file from '%.*s'."
                    "Error message: '%s'",
                    AZ_STRING_ARG(filePath.Native()),
                    readResult.GetError().c_str()
                );
                return InvalidTemplateId;
            }

            return LoadTemplateFromString(readResult.GetValue(), filePath, progressedFilePathsSet);
        }

        TemplateId PrefabLoader::LoadTemplateFromString(
            AZStd::string_view content, AZ::IO::PathView originPath)
        {
            AZStd::unordered_set<AZ::IO::Path> progressedFilePathsSet;
            TemplateId newTemplateId = LoadTemplateFromString(content, originPath, progressedFilePathsSet);
            return newTemplateId;
        }

        TemplateId PrefabLoader::LoadTemplateFromString(
            AZStd::string_view fileContent,
            AZ::IO::PathView originPath,
            AZStd::unordered_set<AZ::IO::Path>& progressedFilePathsSet)
        {
            if (!IsValidPrefabPath(originPath))
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadTemplateFromString - "
                    "Invalid origin path: '%.*s'",
                    AZ_STRING_ARG(originPath.Native())
                );
                return InvalidTemplateId;
            }

            AZ::IO::Path relativePath = GenerateRelativePath(originPath);

            // Cyclical dependency detected if the prefab file is already part of the progressed
            // file path set.
            if (progressedFilePathsSet.contains(relativePath))
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadTemplateFromString - "
                    "Prefab file '%.*s' has been detected to directly or indirectly depend on itself."
                    "Terminating any further loading of this branch of its prefab hierarchy.",
                    AZ_STRING_ARG(originPath.Native())
                );
                return InvalidTemplateId;
            }

            // Directly return loaded Template id.
            TemplateId loadedTemplateId = m_prefabSystemComponentInterface->GetTemplateIdFromFilePath(relativePath);
            if (loadedTemplateId != InvalidTemplateId)
            {
                return loadedTemplateId;
            }

            // Read Template's prefab file from disk and parse Prefab DOM from file.
            AZ::Outcome<PrefabDom, AZStd::string> readPrefabFileResult = AzFramework::FileFunc::ReadJsonFromString(fileContent);
            if (!readPrefabFileResult.IsSuccess())
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadTemplate - Failed to load Prefab file from '%.*s'."
                    "Error message: '%s'",
                    AZ_STRING_ARG(originPath.Native()),
                    readPrefabFileResult.GetError().c_str());

                return InvalidTemplateId;
            }

            // Add or replace the Source parameter in the dom
            PrefabDomPath sourcePath = PrefabDomPath((AZStd::string("/") + PrefabDomUtils::SourceName).c_str());
            sourcePath.Set(readPrefabFileResult.GetValue(), relativePath.Native().c_str());

            // Create new Template with the Prefab DOM.
            TemplateId newTemplateId = m_prefabSystemComponentInterface->AddTemplate(relativePath, readPrefabFileResult.TakeValue());
            if (newTemplateId == InvalidTemplateId)
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadTemplate - "
                    "Failed to create a template from instance with source file path '%.*s': "
                    "invalid template id returned.",
                    AZ_STRING_ARG(originPath.Native())
                );

                return InvalidTemplateId;
            }

            TemplateReference newTemplateReference = m_prefabSystemComponentInterface->FindTemplate(newTemplateId);
            Template& newTemplate = newTemplateReference->get();

            // Mark the file as being in progress.
            progressedFilePathsSet.emplace(relativePath);

            // Get 'Instances' value from Template.
            bool isLoadSuccessful = true;
            PrefabDomValueReference instancesReference = newTemplate.GetInstancesValue();
            if (instancesReference.has_value())
            {
                PrefabDomValue& instances = instancesReference->get();

                // For each instance value in 'instances', try to create source Templates for target Template's nested instance data.
                // Also create Links between source/target Templates if source Template loaded successfully.
                for (PrefabDomValue::MemberIterator instanceIterator = instances.MemberBegin(); instanceIterator != instances.MemberEnd();
                     ++instanceIterator)
                {
                    if (!LoadNestedInstance(instanceIterator, newTemplateId, progressedFilePathsSet))
                    {
                        isLoadSuccessful = false;
                        AZ_Error(
                            "Prefab", false,
                            "PrefabLoader::LoadTemplate - "
                            "Loading nested instance '%s' in target Template '%u' from Prefab file '%.*s' failed.",
                            instanceIterator->name.GetString(), newTemplateId,
                            AZ_STRING_ARG(originPath.Native())
                        );
                    }
                }
            }

            isLoadSuccessful &= SanitizeLoadedTemplate(newTemplate.GetPrefabDom());

            newTemplate.MarkAsLoadedWithErrors(!isLoadSuccessful);

            // Un-mark the file as being in progress.
            progressedFilePathsSet.erase(originPath);

            // Return target Template id.
            return newTemplateId;
        }

        bool PrefabLoader::LoadNestedInstance(
            PrefabDomValue::MemberIterator& instanceIterator, TemplateId targetTemplateId,
            AZStd::unordered_set<AZ::IO::Path>& progressedFilePathsSet)
        {
            const PrefabDomValue& instance = instanceIterator->value;
            AZ::IO::PathView instancePath = AZStd::string_view(instanceIterator->name.GetString(), instanceIterator->name.GetStringLength());

            if (!IsValidPrefabPath(instancePath))
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadNestedInstance - "
                    "There's an Instance with an invalid path '%s' in the target Template on file path '%s'.",
                    instanceIterator->name.GetString(),
                    m_prefabSystemComponentInterface->FindTemplate(targetTemplateId)->get().GetFilePath().c_str());
                return false;
            }

            // Get source Template's path for getting nested instance data.
            PrefabDomValueConstReference sourceReference = PrefabDomUtils::FindPrefabDomValue(instance, PrefabDomUtils::SourceName);
            if (!sourceReference.has_value() || !sourceReference->get().IsString() || sourceReference->get().GetStringLength() == 0)
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadNestedInstance - "
                    "Can't get '%s' string value in Instance value '%s' of Template's Prefab DOM from file '%s'.",
                    PrefabDomUtils::SourceName, instanceIterator->name.GetString(),
                    m_prefabSystemComponentInterface->FindTemplate(targetTemplateId)->get().GetFilePath().c_str());
                return false;
            }
            const PrefabDomValue& source = sourceReference->get();
            AZStd::string_view nestedTemplatePath(source.GetString(), source.GetStringLength());

            // Get Template id of nested instance from its path.
            // If source Template is already loaded, get the id from Template File Path To Id Map,
            // else load the source Template by calling 'LoadTemplate' recursively.

            TemplateId nestedTemplateId = LoadTemplateFromFile(nestedTemplatePath, progressedFilePathsSet);
            TemplateReference nestedTemplateReference = m_prefabSystemComponentInterface->FindTemplate(nestedTemplateId);
            if (!nestedTemplateReference.has_value() || !nestedTemplateReference->get().IsValid())
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadNestedInstance - "
                    "Error occurred while loading nested Prefab file '%.*s' from Prefab file '%s'.",
                    AZ_STRING_ARG(nestedTemplatePath),
                    m_prefabSystemComponentInterface->FindTemplate(targetTemplateId)->get().GetFilePath().c_str()
                );
                return false;
            }

            // After source template has been loaded, create Link between source/target Template.
            LinkId newLinkId =
                m_prefabSystemComponentInterface->AddLink(nestedTemplateId, targetTemplateId, instanceIterator, AZStd::nullopt);
            if (newLinkId == InvalidLinkId)
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::LoadNestedInstance - "
                    "Failed to add a new Link to Nested Template Instance '%s' which connects source Template '%.*s' and target Template "
                    "'%s'.",
                    instanceIterator->name.GetString(), AZ_STRING_ARG(nestedTemplatePath),
                    m_prefabSystemComponentInterface->FindTemplate(targetTemplateId)->get().GetFilePath().c_str()
                );

                return false;
            }

            // Let the new Template carry up the error flag of its nested Prefab.
            return !nestedTemplateReference->get().IsLoadedWithErrors();
        }

        bool PrefabLoader::SanitizeLoadedTemplate(PrefabDomReference loadedTemplateDom)
        {
            // Prefabs are stored to disk with default values stripped. However, while in memory, we need those default values to be
            // present to make patches work consistently. To accomplish this, we'll instantiate the Dom, then serialize the instance
            // back into a Dom with all of the default values preserved.
            // Note that this is the default behavior in Prefab serialization, so we don't need to specify StoreInstanceFlags.

            if (!loadedTemplateDom)
            {
                return false;
            }

            Instance loadedPrefabInstance;
            if (!PrefabDomUtils::LoadInstanceFromPrefabDom(loadedPrefabInstance, loadedTemplateDom->get()))
            {
                return false;
            }

            PrefabDom storedPrefabDom(&loadedTemplateDom->get().GetAllocator());
            if (!PrefabDomUtils::StoreInstanceInPrefabDom(loadedPrefabInstance, storedPrefabDom, PrefabDomUtils::StoreFlags::StoreLinkIds))
            {
                return false;
            }

            loadedTemplateDom->get().CopyFrom(storedPrefabDom, loadedTemplateDom->get().GetAllocator());
            return true;
        }

        bool PrefabLoader::SanitizeSavingTemplate(PrefabDomReference savingTemplateDom)
        {
            // Prefabs are stored in memory with default values spelled out to make patches work consistently. However, when we store them
            // to disk, we strip those default values to save on file size. To accomplish this, we'll instantiate the Dom, then serialize
            // the instance back into a Dom with all of the default values stripped.

            if (!savingTemplateDom)
            {
                return false;
            }

            Instance savingPrefabInstance;
            if (!PrefabDomUtils::LoadInstanceFromPrefabDom(savingPrefabInstance, savingTemplateDom->get()))
            {
                return false;
            }

            PrefabDom storedPrefabDom(&savingTemplateDom->get().GetAllocator());
            if (!PrefabDomUtils::StoreInstanceInPrefabDom(savingPrefabInstance, storedPrefabDom,
                PrefabDomUtils::StoreFlags::StripDefaultValues))
            {
                return false;
            }

            savingTemplateDom->get().CopyFrom(storedPrefabDom, savingTemplateDom->get().GetAllocator());

            return true;
        }

        bool PrefabLoader::SaveTemplate(TemplateId templateId)
        {
            const auto& domAndFilepath = StoreTemplateIntoFileFormat(templateId);
            if (!domAndFilepath)
            {
                return false;
            }

            auto outcome = AzFramework::FileFunc::WriteJsonFile(domAndFilepath->first, GetFullPath(domAndFilepath->second));
            if (!outcome.IsSuccess())
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::SaveTemplate - "
                    "Failed to save template '%s'."
                    "Error: %s",
                    domAndFilepath->second.c_str(),
                    outcome.GetError().c_str()
                );
                return false;
            }
            m_prefabSystemComponentInterface->SetTemplateDirtyFlag(templateId, false);
            return true;
        }

        bool PrefabLoader::SaveTemplateToFile(TemplateId templateId, AZ::IO::PathView absolutePath)
        {
            AZ_Assert(absolutePath.IsAbsolute(), "SaveTemplateToFile requires an absolute path for saving the initial prefab file.");

            const auto& domAndFilepath = StoreTemplateIntoFileFormat(templateId);
            if (!domAndFilepath)
            {
                return false;
            }

            // Verify that the absolute path provided to this matches the relative path saved in the template.
            // Otherwise, the saved prefab won't be able to be loaded.
            auto relativePath = GenerateRelativePath(absolutePath);
            if (relativePath != domAndFilepath->second)
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::SaveTemplateToFile - "
                    "Failed to save template '%s' to location '%.*s'."
                    "Error: Relative path '%.*s' for location didn't match template name.",
                    domAndFilepath->second.c_str(), AZ_STRING_ARG(absolutePath.Native()), AZ_STRING_ARG(relativePath.Native()));
                return false;
            }

            auto outcome = AzFramework::FileFunc::WriteJsonFile(domAndFilepath->first, absolutePath);
            if (!outcome.IsSuccess())
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::SaveTemplateToFile - "
                    "Failed to save template '%s' to location '%.*s'."
                    "Error: %s",
                    domAndFilepath->second.c_str(), AZ_STRING_ARG(absolutePath.Native()), outcome.GetError().c_str());
                return false;
            }
            m_prefabSystemComponentInterface->SetTemplateDirtyFlag(templateId, false);
            return true;
        }

        bool PrefabLoader::SaveTemplateToString(TemplateId templateId, AZStd::string& output)
        {
            const auto& domAndFilepath = StoreTemplateIntoFileFormat(templateId);
            if (!domAndFilepath)
            {
                return false;
            }

            auto outcome = AzFramework::FileFunc::WriteJsonToString(domAndFilepath->first, output);
            if (!outcome.IsSuccess())
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::SaveTemplateToString - "
                    "Failed to serialize template '%s' into a string."
                    "Error: %s",
                    domAndFilepath->second.c_str(),
                    outcome.GetError().c_str()
                );
                return false;
            }
            return true;
        }

        AZStd::optional<AZStd::pair<PrefabDom, AZ::IO::Path>> PrefabLoader::StoreTemplateIntoFileFormat(TemplateId templateId)
        {
            // Acquire the template we are saving
            TemplateReference templateToSaveReference = m_prefabSystemComponentInterface->FindTemplate(templateId);
            if (!templateToSaveReference.has_value())
            {
                AZ_Warning(
                    "Prefab", false,
                    "PrefabLoader::SaveTemplate - Unable to save prefab template with id: '%llu'. "
                    "Template with that id could not be found",
                    templateId
                );

                return AZStd::nullopt;
            }

            Template& templateToSave = templateToSaveReference->get();
            if (!templateToSave.IsValid())
            {
                AZ_Warning(
                    "Prefab", false,
                    "PrefabLoader::SaveTemplate - Unable to save Prefab Template with id: %llu. "
                    "Template with that id is invalid",
                    templateId);

                return AZStd::nullopt;
            }

            // Make a copy of a our prefab DOM where nested instances become file references with patch data
            PrefabDom templateDomToSave;
            if (!CopyTemplateIntoPrefabFileFormat(templateToSave, templateDomToSave))
            {
                AZ_Error(
                    "Prefab", false,
                    "PrefabLoader::SaveTemplate - Unable to store a collapsed version of prefab Template while attempting to save to %s."
                    "Save cannot continue",
                    templateToSave.GetFilePath().c_str()
                );

                return AZStd::nullopt;
            }

            return { { AZStd::move(templateDomToSave), templateToSave.GetFilePath() } };
        }

        bool PrefabLoader::CopyTemplateIntoPrefabFileFormat(TemplateReference templateRef, PrefabDom& output)
        {
            AZ_Assert(
                templateRef.has_value(),
                "CopyTemplateIntoPrefabFileFormat called on empty template reference."
            );

            PrefabDom& prefabDom = templateRef->get().GetPrefabDom();

            // Start by making a copy of our dom
            output.CopyFrom(prefabDom, prefabDom.GetAllocator());

            SanitizeSavingTemplate(output);

            for (const LinkId& linkId : templateRef->get().GetLinks())
            {
                AZStd::optional<AZStd::reference_wrapper<Link>> findLinkResult = m_prefabSystemComponentInterface->FindLink(linkId);

                if (!findLinkResult.has_value())
                {
                    AZ_Error(
                        "Prefab", false,
                        "Link with id %llu could not be found while attempting to store "
                        "Prefab Template with source path %s in Prefab File format. "
                        "Unable to proceed.",
                        linkId, templateRef->get().GetFilePath().c_str());

                    return false;
                }

                if (!findLinkResult->get().IsValid())
                {
                    AZ_Error(
                        "Prefab", false,
                        "Link with id %llu and is invalid during attempt to store "
                        "Prefab Template with source path %s in Prefab File format. "
                        "Unable to Proceed.",
                        linkId, templateRef->get().GetFilePath().c_str());

                    return false;
                }

                Link& link = findLinkResult->get();

                PrefabDomPath instancePath = link.GetInstancePath();
                PrefabDom& linkDom = link.GetLinkDom();

                // Get the instance value of the Template copy
                // This currently stores a fully realized nested Template Dom
                PrefabDomValue* instanceValue = instancePath.Get(output);

                if (!instanceValue)
                {
                    AZ_Error(
                        "Prefab", false,
                        "Template::CopyTemplateIntoPrefabFileFormat: Unable to recover nested instance Dom value from link with id %llu "
                        "while attempting to store a collapsed version of a Prefab Template with source path %s. Unable to proceed.",
                        linkId, templateRef->get().GetFilePath().c_str());

                    return false;
                }

                // Copy the contents of the Link to overwrite our Template Dom copies Instance
                // The instance is now "collapsed" as it contains the file reference and patches from the link
                instanceValue->CopyFrom(linkDom, prefabDom.GetAllocator());
            }

            // Remove Source parameter from the dom. It will be added on file load, and should not be stored to disk.
            PrefabDomPath sourcePath = PrefabDomPath((AZStd::string("/") + PrefabDomUtils::SourceName).c_str());
            sourcePath.Erase(output);

            return true;
        }

        bool PrefabLoader::IsValidPrefabPath(AZ::IO::PathView path)
        {
            // Check for OS invalid character and paths ending on '/' '\\' separators as final char
            AZStd::string_view pathStr = path.Native();

            return !path.empty() &&
                (pathStr.find_first_of(AZ_FILESYSTEM_INVALID_CHARACTERS) == AZStd::string::npos) &&
                (pathStr.back() != '\\' && pathStr.back() != '/');
        }

        AZ::IO::Path PrefabLoader::GetFullPath(AZ::IO::PathView path)
        {
            AZ::IO::Path pathWithOSSeparator = AZ::IO::Path(path).MakePreferred();
            if (pathWithOSSeparator.IsAbsolute())
            {
                // If an absolute path was passed in, just return it as-is.
                return path;
            }

            // A relative path was passed in, so try to turn it back into an absolute path.

            AZ::IO::Path fullPath;

            bool pathFound = false;
            AZ::Data::AssetInfo assetInfo;
            AZStd::string rootFolder;
            AZStd::string inputPath(path.Native());

            // Given an input path that's expected to exist, try to look it up.
            AzToolsFramework::AssetSystemRequestBus::BroadcastResult(
                pathFound, &AzToolsFramework::AssetSystemRequestBus::Events::GetSourceInfoBySourcePath,
                inputPath.c_str(), assetInfo, rootFolder);

            if (pathFound)
            {
                // The asset system provided us with a valid root folder and relative path, so return it.
                fullPath = AZ::IO::Path(rootFolder) / assetInfo.m_relativePath;
            }
            else
            {
                // If for some reason the Asset system couldn't provide a relative path, provide some fallback logic.

                // Check to see if the AssetProcessor is ready.  If it *is* and we didn't get a path, print an error then follow
                // the fallback logic.  If it's *not* ready, we're probably either extremely early in a tool startup flow or inside
                // a unit test, so just execute the fallback logic without an error.
                [[maybe_unused]] bool assetProcessorReady = false;
                AzFramework::AssetSystemRequestBus::BroadcastResult(
                    assetProcessorReady, &AzFramework::AssetSystemRequestBus::Events::AssetProcessorIsReady);

                AZ_Error(
                    "Prefab", !assetProcessorReady, "Full source path for '%.*s' could not be determined. Using fallback logic.",
                    AZ_STRING_ARG(path.Native()));

                // If a relative path was passed in, make it relative to the project root.
                fullPath = AZ::IO::Path(m_projectPathWithOsSeparator).Append(pathWithOSSeparator);
            }

            return fullPath;
        }

        AZ::IO::Path PrefabLoader::GenerateRelativePath(AZ::IO::PathView path)
        {
            bool pathFound = false;

            AZStd::string relativePath;
            AZStd::string rootFolder;
            AZ::IO::Path finalPath;

            // The asset system allows for paths to be relative to multiple root folders, using a priority system.
            // This request will make the input path relative to the most appropriate, highest-priority root folder.
            AzToolsFramework::AssetSystemRequestBus::BroadcastResult(
                pathFound, &AzToolsFramework::AssetSystemRequestBus::Events::GenerateRelativeSourcePath, path.Native(),
                relativePath, rootFolder);

            if (pathFound && !relativePath.empty())
            {
                // A relative path was generated successfully, so return it.
                finalPath = relativePath;
            }
            else
            {
                // If for some reason the Asset system couldn't provide a relative path, provide some fallback logic.

                // Check to see if the AssetProcessor is ready.  If it *is* and we didn't get a path, print an error then follow
                // the fallback logic.  If it's *not* ready, we're probably either extremely early in a tool startup flow or inside
                // a unit test, so just execute the fallback logic without an error.
                [[maybe_unused]] bool assetProcessorReady = false;
                AzFramework::AssetSystemRequestBus::BroadcastResult(
                    assetProcessorReady, &AzFramework::AssetSystemRequestBus::Events::AssetProcessorIsReady);

                AZ_Error("Prefab", !assetProcessorReady,
                    "Relative source path for '%.*s' could not be determined. Using project path as relative root.",
                    AZ_STRING_ARG(path.Native()));

                AZ::IO::Path pathWithOSSeparator = AZ::IO::Path(path.Native()).MakePreferred();

                if (pathWithOSSeparator.IsAbsolute())
                {
                    // If an absolute path was passed in, make it relative to the project path.
                    finalPath = AZ::IO::Path(path.Native(), '/').MakePreferred().LexicallyRelative(m_projectPathWithSlashSeparator);
                }
                else
                {
                    // If a relative path was passed in, just return it.
                    finalPath = path;
                }
            }

            return finalPath;
        }

        AZ::IO::Path PrefabLoaderInterface::GeneratePath()
        {
            return AZStd::string::format("Prefab_%s", AZ::Entity::MakeId().ToString().c_str());
        }

    } // namespace Prefab
} // namespace AzToolsFramework
