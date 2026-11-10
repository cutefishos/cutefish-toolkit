// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/frame_client.h"

#include "ui/window.h"

namespace cutefish::ui {

FrameClient::FrameClient(Window* window)
    : m_window(window)
{
    if (m_window)
        m_window->addFrameClient(this);
}

FrameClient::~FrameClient()
{
    if (m_window)
        m_window->removeFrameClient(this);
}

void FrameClient::startTicking()
{
    if (m_ticking)
        return;
    m_ticking = true;
    if (m_window)
        m_window->scheduleFrame();
}

void FrameClient::stopTicking()
{
    m_ticking = false;
}

void FrameClient::deferHover()
{
    if (m_window)
        m_window->deferHover();
}

} // namespace cutefish::ui
