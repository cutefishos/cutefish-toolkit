// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "base/event_loop.h"
#include "ui/application.h"
#include "ui/text_configuration.h"

cutefish::base::EventLoop* g_hostLoop = nullptr;

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    cutefish::ui::setTextConfigurationFile(CUTEFISH_TOOLKIT_TEST_FONTS_CONF);
    // As the compositor does: the host's loop exists first and is shared.
    cutefish::base::EventLoop hostLoop;
    g_hostLoop = &hostLoop;
    cutefish::ui::Application application(argc, argv,
                                          cutefish::ui::Application::Platform::Embedded);
    return RUN_ALL_TESTS();
}
