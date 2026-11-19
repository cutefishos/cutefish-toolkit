// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <cstdio>
#include <cstring>

#include <gtest/gtest.h>

#include "ui/application.h"
#include "ui/test_input.h"
#include "ui/text_configuration.h"

int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    cutefish::ui::setTextConfigurationFile(CUTEFISH_TOOLKIT_TEST_FONTS_CONF);
    // --verify-layout: every pass is checked against a full one.
    bool verifiesLayout = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verify-layout") == 0)
            verifiesLayout = true;
    }
    cutefish::ui::Application application(argc, argv,
                                          cutefish::ui::Application::Platform::Headless);
    cutefish::ui::TestInput::setVerifiesLayout(verifiesLayout);
    const int result = RUN_ALL_TESTS();
    if (verifiesLayout)
        printf("%d views laid out differently in full\n", cutefish::ui::TestInput::layoutMismatches());
    return result;
}
