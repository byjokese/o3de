/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "AtomOutputFrameCapture.h"

#include <Atom/RPI.Public/Pass/Specific/RenderToTexturePass.h>
#include <Atom/RPI.Public/RenderPipeline.h>
#include <Atom/RPI.Public/Scene.h>
#include <Atom/RPI.Public/View.h>
#include <Atom/RPI.Reflect/System/RenderPipelineDescriptor.h>
#include <AzCore/Component/TransformBus.h>
#include <AzCore/Math/MatrixUtils.h>
#include <AzCore/Name/Name.h>
#include <AzFramework/Entity/GameEntityContextBus.h>
#include <AzFramework/Scene/Scene.h>
#include <AzFramework/Scene/SceneSystemInterface.h>

namespace TrackView
{
    void AtomOutputFrameCapture::CreatePipeline(
        AZ::RPI::Scene& scene, const AZStd::string& pipelineName, const uint32_t width, const uint32_t height)
    {
        AZ::RPI::RenderPipelineDescriptor pipelineDesc;
        pipelineDesc.m_mainViewTagName = "MainCamera"; // must be "MainCamera"
        pipelineDesc.m_name = pipelineName;
        pipelineDesc.m_rootPassTemplate = "MainPipelineRenderToTexture";
        pipelineDesc.m_renderSettings.m_multisampleState.m_samples = 4;
        m_renderPipeline = AZ::RPI::RenderPipeline::CreateRenderPipeline(pipelineDesc);

        if (auto renderToTexturePass = azrtti_cast<AZ::RPI::RenderToTexturePass*>(m_renderPipeline->GetRootPass().get()))
        {
            renderToTexturePass->ResizeOutput(width, height);
        }

        scene.AddRenderPipeline(m_renderPipeline);

        // rendering pipeline has a tree structure
        m_passHierarchy.push_back(pipelineName);
        m_passHierarchy.push_back("CopyToSwapChain");

        // retrieve View from the camera that's animating
        AZ::Name viewName = AZ::Name("MainCamera");
        m_view = AZ::RPI::View::CreateView(viewName, AZ::RPI::View::UsageCamera);
        m_renderPipeline->SetDefaultView(m_view);
    }

    void AtomOutputFrameCapture::DestroyPipeline(AZ::RPI::Scene& scene)
    {
        scene.RemoveRenderPipeline(m_renderPipeline->GetId());
        m_passHierarchy.clear();
        m_renderPipeline.reset();
        m_view.reset();
    }

    void AtomOutputFrameCapture::UpdateView(const AZ::Matrix3x4& cameraTransform, const AZ::Matrix4x4& cameraProjection)
    {
        m_view->SetCameraTransform(cameraTransform);
        m_view->SetViewToClipMatrix(cameraProjection);
    }

    bool AtomOutputFrameCapture::BeginCapture(
        const AZ::RPI::AttachmentReadback::CallbackFunction& attachmentReadbackCallback, CaptureFinishedCallback captureFinishedCallback)
    {
        AZ::Render::FrameCaptureNotificationBus::Handler::BusConnect();

        m_captureFinishedCallback = AZStd::move(captureFinishedCallback);

        // note: "Output" (slot name) maps to MainPipeline.pass CopyToSwapChain
        bool startedCapture = false;
        AZ::Render::FrameCaptureRequestBus::BroadcastResult(
            startedCapture, &AZ::Render::FrameCaptureRequestBus::Events::CapturePassAttachmentWithCallback, m_passHierarchy,
            AZStd::string("Output"), attachmentReadbackCallback, AZ::RPI::PassAttachmentReadbackOption::Output);

        return startedCapture;
    }

    void AtomOutputFrameCapture::OnCaptureFinished(
        [[maybe_unused]] AZ::Render::FrameCaptureResult result, [[maybe_unused]] const AZStd::string& info)
    {
        m_captureFinishedCallback();
        AZ::Render::FrameCaptureNotificationBus::Handler::BusDisconnect();
    }

    AZ::Matrix3x4 TransformFromEntityId(const AZ::EntityId entityId)
    {
        AZ::Transform cameraTransform = AZ::Transform::CreateIdentity();
        AZ::TransformBus::EventResult(cameraTransform, entityId, &AZ::TransformBus::Events::GetWorldTM);
        return AZ::Matrix3x4::CreateFromTransform(cameraTransform);
    }

    AZ::Matrix4x4 ProjectionFromCameraEntityId(const AZ::EntityId entityId, const float outputWidth, const float outputHeight)
    {
        float nearDist = 0.0f;
        Camera::CameraRequestBus::EventResult(nearDist, entityId, &Camera::CameraRequestBus::Events::GetNearClipDistance);
        float farDist = 0.0f;
        Camera::CameraRequestBus::EventResult(farDist, entityId, &Camera::CameraRequestBus::Events::GetFarClipDistance);
        float fovRad = 0.0f;
        Camera::CameraRequestBus::EventResult(fovRad, entityId, &Camera::CameraRequestBus::Events::GetFovRadians);

        const float aspectRatio = outputWidth / outputHeight;

        AZ::Matrix4x4 viewToClipMatrix;
        AZ::MakePerspectiveFovMatrixRH(viewToClipMatrix, fovRad, aspectRatio, nearDist, farDist, /*reverseDepth=*/true);
        return viewToClipMatrix;
    }

    AZ::RPI::Scene* SceneFromGameEntityContext()
    {
        AzFramework::EntityContextId entityContextId;
        AzFramework::GameEntityContextRequestBus::BroadcastResult(
            entityContextId, &AzFramework::GameEntityContextRequestBus::Events::GetGameEntityContextId);

        return AZ::RPI::Scene::GetSceneForEntityContextId(entityContextId);
    }
} // namespace TrackView
