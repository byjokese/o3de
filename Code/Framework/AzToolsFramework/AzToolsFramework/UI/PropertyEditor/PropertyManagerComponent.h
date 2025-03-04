/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#ifndef PROPERTY_MANAGER_COMPONENT_H
#define PROPERTY_MANAGER_COMPONENT_H

#include <AzCore/base.h>
#include <AzCore/std/containers/map.h>
#include <AzCore/Memory/SystemAllocator.h>
#include <AzCore/Component/Component.h>
#include "PropertyEditorAPI.h"

// the property manager component's job is to provide the services for registration of property editors.
// it also registers all of our built-in property manager types
namespace AzToolsFramework
{
    namespace Components
    {
        class PropertyManagerComponent
            : public AZ::Component
            , private PropertyTypeRegistrationMessages::Bus::Handler
        {
        public:
            friend class PropertyManagerComponentFactory;

            AZ_COMPONENT(PropertyManagerComponent, "{0BBDF87F-DDA8-460D-9861-93260BC5C5A9}")

            PropertyManagerComponent();
            virtual ~PropertyManagerComponent();

            //////////////////////////////////////////////////////////////////////////
            // AZ::Component
            virtual void Init();
            virtual void Activate();
            virtual void Deactivate();

            virtual PropertyHandlerBase* ResolvePropertyHandler(AZ::u32 handlerName, const AZ::Uuid& handlerType) override;

            //////////////////////////////////////////////////////////////////////////
        private:

            static void GetProvidedServices(AZ::ComponentDescriptor::DependencyArrayType& provided)
            {
                provided.push_back(AZ_CRC("PropertyManagerService", 0x63a3d7ad));
            }

            static void GetIncompatibleServices(AZ::ComponentDescriptor::DependencyArrayType& incompatible)
            {
                incompatible.push_back(AZ_CRC("PropertyManagerService", 0x63a3d7ad));
            }

            static void Reflect(AZ::ReflectContext* context);

            //////////////////////////////////////////////////////////////////////////
            // PropertyTypeRegistrationMessages::Bus::Handler
            virtual void RegisterPropertyType(PropertyHandlerBase* pHandler) override;
            virtual void UnregisterPropertyType(PropertyHandlerBase* pHandler) override;
            //////////////////////////////////////////////////////////////////////////

            typedef AZStd::unordered_multimap<AZ::u32, PropertyHandlerBase*> HandlerMap;
            typedef AZStd::unordered_multimap<AZ::Uuid, PropertyHandlerBase*> DefaultHandlerMap;

            HandlerMap m_Handlers;
            DefaultHandlerMap m_DefaultHandlers;

            AZStd::vector<PropertyHandlerBase*> m_builtInHandlers; // exists purely to delete them later.

            void CreateBuiltInHandlers();
        };
    } // namespace Components
} // namespace AzToolsFramework

#endif
