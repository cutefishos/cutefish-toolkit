// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <string>

#include "base/event_loop.h"
#include "base/file.h"
#include "base/process.h"
#include "base/timer.h"

using namespace cutefish::base;
using namespace std::chrono_literals;

namespace {

// Runs the loop until the process ends or five seconds pass.
bool runUntilFinished(EventLoop& loop, Process& process, int* exitCode = nullptr, bool* crashed = nullptr)
{
    bool finished = false;
    process.onFinished = [&](int code, bool killed) {
        finished = true;
        if (exitCode)
            *exitCode = code;
        if (crashed)
            *crashed = killed;
        loop.quit();
    };
    Timer timeout(&loop);
    timeout.setSingleShot(true);
    timeout.onTimeout = [&loop] { loop.quit(); };
    timeout.start(5s);
    loop.run();
    return finished;
}

} // namespace

TEST(ProcessTest, CollectsOutputAndExitCode)
{
    EventLoop loop;
    Process process(&loop);
    ASSERT_TRUE(process.start("sh", {"-c", "printf 'out'; printf 'err' >&2; exit 3"}));
    EXPECT_TRUE(process.isRunning());
    int exitCode = -1;
    bool crashed = true;
    ASSERT_TRUE(runUntilFinished(loop, process, &exitCode, &crashed));
    EXPECT_EQ(exitCode, 3);
    EXPECT_FALSE(crashed);
    EXPECT_EQ(process.standardOutput(), "out");
    EXPECT_EQ(process.standardError(), "err");
    EXPECT_FALSE(process.isRunning());
}

TEST(ProcessTest, WritesInputAndRunsInTheWorkingDirectory)
{
    EventLoop loop;
    Process process(&loop);
    process.setStandardInput(std::string(200000, 'x'));
    process.setWorkingDirectory("/tmp");
    ASSERT_TRUE(process.start("sh", {"-c", "wc -c; pwd"}));
    ASSERT_TRUE(runUntilFinished(loop, process));
    EXPECT_EQ(process.standardOutput(), "200000\n/tmp\n");
}

TEST(ProcessTest, ReportsSignals)
{
    EventLoop loop;
    Process process(&loop);
    ASSERT_TRUE(process.start("sleep", {"30"}));
    EXPECT_TRUE(process.terminate());
    int signal = 0;
    bool crashed = false;
    ASSERT_TRUE(runUntilFinished(loop, process, &signal, &crashed));
    EXPECT_TRUE(crashed);
    EXPECT_EQ(signal, SIGTERM);
}

TEST(ProcessTest, RefusesMissingPrograms)
{
    EventLoop loop;
    Process process(&loop);
    EXPECT_FALSE(process.start("cutefish-no-such-program"));
    EXPECT_FALSE(process.isRunning());
}

TEST(ProcessTest, StartsDetachedInASessionOfItsOwn)
{
    EventLoop loop;
    const pid_t pid = Process::startDetached("sleep", {"0.1"});
    ASSERT_GT(pid, 0);
    EXPECT_EQ(getsid(pid), pid);
    // Reaped by the loop once it ends.
    Timer wait(&loop);
    wait.setSingleShot(true);
    wait.onTimeout = [&loop] { loop.quit(); };
    wait.start(500ms);
    loop.run();
    EXPECT_NE(kill(pid, 0), 0);
}

TEST(ProcessTest, UsesTheGivenEnvironment)
{
    EventLoop loop;
    Process process(&loop);
    process.setEnvironment({"CUTEFISH_TEST_VALUE=given", "PATH=/usr/bin:/bin"});
    ASSERT_TRUE(process.start("sh", {"-c", "printf '%s' \"$CUTEFISH_TEST_VALUE\"; printf '%s' \"${HOME:-none}\""}));
    ASSERT_TRUE(runUntilFinished(loop, process));
    EXPECT_EQ(process.standardOutput(), "givennone");
}

TEST(ProcessTest, PassesOneDescriptorAndClosesTheRest)
{
    EventLoop loop;
    int pipe[2];
    ASSERT_EQ(::pipe(pipe), 0);
    // Open without close-on-exec: the child must still not get it.
    const int stray = ::open("/dev/null", O_RDONLY);
    ASSERT_GT(stray, Process::kInheritedFd);
    Process process(&loop);
    process.setInheritedFd(pipe[1]);
    const std::string script = "printf 'via3' >&3; if [ -e /proc/self/fd/" + std::to_string(stray)
            + " ]; then printf open; else printf closed; fi";
    ASSERT_TRUE(process.start("sh", {"-c", script}));
    ::close(pipe[1]);
    ASSERT_TRUE(runUntilFinished(loop, process));
    char buffer[16] = {};
    ASSERT_EQ(::read(pipe[0], buffer, sizeof(buffer)), 4);
    EXPECT_STREQ(buffer, "via3");
    EXPECT_EQ(process.standardOutput(), "closed");
    ::close(pipe[0]);
    ::close(stray);
}

TEST(ProcessTest, ForwardsOutput)
{
    EventLoop loop;
    int pipe[2];
    ASSERT_EQ(::pipe(pipe), 0);
    const int savedOut = ::dup(STDOUT_FILENO);
    const int savedErr = ::dup(STDERR_FILENO);
    ::dup2(pipe[1], STDOUT_FILENO);
    ::dup2(pipe[1], STDERR_FILENO);
    ::close(pipe[1]);
    Process process(&loop);
    process.setOutputForwarded(true);
    std::string seen;
    process.onStandardOutput = [&](std::string_view text) { seen += text; };
    const bool started = process.start("sh", {"-c", "printf out; printf err >&2"});
    const bool finished = started && process.waitForFinished(5s);
    ::dup2(savedOut, STDOUT_FILENO);
    ::dup2(savedErr, STDERR_FILENO);
    ::close(savedOut);
    ::close(savedErr);
    ASSERT_TRUE(finished);
    char buffer[16] = {};
    ASSERT_EQ(::read(pipe[0], buffer, sizeof(buffer)), 6);
    EXPECT_STREQ(buffer, "outerr");
    EXPECT_TRUE(seen.empty());
    EXPECT_TRUE(process.standardOutput().empty());
    ::close(pipe[0]);
}

TEST(ProcessTest, ForwardsOnlyErrors)
{
    EventLoop loop;
    int pipe[2];
    ASSERT_EQ(::pipe(pipe), 0);
    const int savedErr = ::dup(STDERR_FILENO);
    ::dup2(pipe[1], STDERR_FILENO);
    ::close(pipe[1]);
    Process process(&loop);
    process.setStandardErrorForwarded(true);
    std::string seen;
    process.onStandardOutput = [&](std::string_view text) { seen += text; };
    const bool started = process.start("sh", {"-c", "printf out; printf err >&2"});
    const bool finished = started && runUntilFinished(loop, process);
    ::dup2(savedErr, STDERR_FILENO);
    ::close(savedErr);
    ASSERT_TRUE(finished);
    char buffer[16] = {};
    ASSERT_EQ(::read(pipe[0], buffer, sizeof(buffer)), 3);
    EXPECT_STREQ(buffer, "err");
    EXPECT_EQ(seen, "out");
    EXPECT_EQ(process.standardOutput(), "out");
    EXPECT_TRUE(process.standardError().empty());
    ::close(pipe[0]);
}

TEST(ProcessTest, WaitForFinishedTimesOutThenCompletes)
{
    EventLoop loop;
    Process process(&loop);
    int exitCode = -1;
    process.onFinished = [&](int code, bool) { exitCode = code; };
    ASSERT_TRUE(process.start("sleep", {"5"}));
    const auto before = std::chrono::steady_clock::now();
    EXPECT_FALSE(process.waitForFinished(100ms));
    EXPECT_GE(std::chrono::steady_clock::now() - before, 100ms);
    EXPECT_TRUE(process.isRunning());
    process.terminate();
    EXPECT_TRUE(process.waitForFinished(2s));
    EXPECT_FALSE(process.isRunning());
    EXPECT_EQ(exitCode, SIGTERM);
    EXPECT_TRUE(process.waitForFinished(0ms));
}

// As the screen recorder: it reads commands until "stop" or the input's end.
TEST(ProcessTest, KeepsTheInputOpenUntilClosed)
{
    EventLoop loop;
    Process process(&loop);
    process.setStandardInput("first\n");
    process.setStandardInputOpen(true);
    ASSERT_TRUE(process.start("sh", {"-c", "while read line; do echo \"got $line\"; [ \"$line\" = stop ] && break; done; echo end"}));
    // Still running after the initial data: the input is open.
    Timer later(&loop);
    later.setSingleShot(true);
    later.onTimeout = [&] {
        EXPECT_TRUE(process.isRunning());
        EXPECT_TRUE(process.writeStandardInput(std::string(100000, 'y') + "\n"));
        EXPECT_TRUE(process.writeStandardInput("stop\n"));
    };
    later.start(200ms);
    ASSERT_TRUE(runUntilFinished(loop, process));
    EXPECT_EQ(process.standardOutput(), "got first\ngot " + std::string(100000, 'y') + "\ngot stop\nend\n");
    EXPECT_FALSE(process.writeStandardInput("late\n"));

    // The end of the input ends it too.
    Process reader(&loop);
    reader.setStandardInputOpen(true);
    ASSERT_TRUE(reader.start("sh", {"-c", "cat; echo eof"}));
    EXPECT_TRUE(reader.writeStandardInput("text\n"));
    reader.closeStandardInput();
    EXPECT_FALSE(reader.writeStandardInput("more\n"));
    ASSERT_TRUE(runUntilFinished(loop, reader));
    EXPECT_EQ(reader.standardOutput(), "text\neof\n");

    // A program that stopped reading: no SIGPIPE here.
    Process quitter(&loop);
    quitter.setStandardInputOpen(true);
    ASSERT_TRUE(quitter.start("true"));
    ASSERT_TRUE(runUntilFinished(loop, quitter));
    Process slow(&loop);
    slow.setStandardInputOpen(true);
    ASSERT_TRUE(slow.start("sh", {"-c", "exec 0<&-; sleep 0.3"}));
    Timer write(&loop);
    write.setSingleShot(true);
    write.onTimeout = [&] { slow.writeStandardInput("ignored\n"); };
    write.start(100ms);
    ASSERT_TRUE(runUntilFinished(loop, slow));
}

// More than a pipe holds, both ways: waiting must keep the pipes moving.
TEST(ProcessTest, WaitForFinishedKeepsReadingAndWriting)
{
    EventLoop loop;
    Process writer(&loop);
    ASSERT_TRUE(writer.start("sh", {"-c", "head -c 1000000 /dev/zero; head -c 300000 /dev/zero >&2; exit 4"}));
    int exitCode = -1;
    writer.onFinished = [&](int code, bool) { exitCode = code; };
    ASSERT_TRUE(writer.waitForFinished(5s));
    EXPECT_EQ(exitCode, 4);
    EXPECT_EQ(writer.standardOutput().size(), 1000000u);
    EXPECT_EQ(writer.standardError().size(), 300000u);

    Process copier(&loop);
    const std::string input(1000000, 'x');
    copier.setStandardInput(input);
    ASSERT_TRUE(copier.start("cat"));
    ASSERT_TRUE(copier.waitForFinished(5s));
    EXPECT_EQ(copier.standardOutput(), input);
}

TEST(ProcessTest, DropsOnlyANegativeOomScore)
{
    file::TemporaryDirectory directory(std::filesystem::temp_directory_path().string());
    ASSERT_TRUE(directory.isValid());
    const std::string protectedScore = directory.path() + "/protected";
    const std::string zero = directory.path() + "/zero";
    const std::string raised = directory.path() + "/raised";
    std::ofstream(protectedScore) << "-900\n";
    std::ofstream(zero) << "0\n";
    std::ofstream(raised) << "300\n";

    dropOomProtection(protectedScore.c_str());
    dropOomProtection(zero.c_str());
    dropOomProtection(raised.c_str());
    dropOomProtection((directory.path() + "/missing").c_str());

    EXPECT_EQ(file::read(protectedScore), "0");
    EXPECT_EQ(file::read(zero), "0\n");
    EXPECT_EQ(file::read(raised), "300\n");
    EXPECT_FALSE(std::filesystem::exists(directory.path() + "/missing"));
}

// Without privilege the test cannot go below 0; a raised score shows that the
// programs inherit it and that nothing lowers it.
TEST(ProcessTest, ProgramsKeepARaisedOomScore)
{
    std::ofstream("/proc/self/oom_score_adj") << "250";
    ASSERT_EQ(file::read("/proc/self/oom_score_adj"), "250\n");

    EventLoop loop;
    for (const bool dropped : {false, true}) {
        Process process(&loop);
        process.setOomProtectionDropped(dropped);
        ASSERT_TRUE(process.start("cat", {"/proc/self/oom_score_adj"}));
        ASSERT_TRUE(runUntilFinished(loop, process));
        EXPECT_EQ(process.standardOutput(), "250\n");
    }

    file::TemporaryDirectory directory(std::filesystem::temp_directory_path().string());
    const std::string written = directory.path() + "/score";
    ASSERT_GT(Process::startDetached("sh", {"-c", "cat /proc/self/oom_score_adj > " + written + ".part; mv "
                                                          + written + ".part " + written}),
              0);
    Timer poll(&loop);
    poll.onTimeout = [&] {
        if (std::filesystem::exists(written))
            loop.quit();
    };
    poll.start(10ms);
    Timer timeout(&loop);
    timeout.setSingleShot(true);
    timeout.onTimeout = [&loop] { loop.quit(); };
    timeout.start(5s);
    loop.run();
    EXPECT_EQ(file::read(written), "250\n");
}

TEST(ProcessTest, DetachedProgramsGetTheGivenEnvironment)
{
    EventLoop loop;
    file::TemporaryDirectory directory(std::filesystem::temp_directory_path().string());
    const std::string written = directory.path() + "/environment";
    ASSERT_GT(Process::startDetached("sh",
                                     {"-c", "printf '%s %s %s' \"$CUTEFISH_GIVEN\" \"$HOME\" \"${PATH:+path}\" > "
                                                    + written + ".part; mv " + written + ".part " + written},
                                     {}, {"CUTEFISH_GIVEN=token", "HOME=/given"}),
              0);
    Timer poll(&loop);
    poll.onTimeout = [&] {
        if (std::filesystem::exists(written))
            loop.quit();
    };
    poll.start(10ms);
    Timer timeout(&loop);
    timeout.setSingleShot(true);
    timeout.onTimeout = [&loop] { loop.quit(); };
    timeout.start(5s);
    loop.run();
    EXPECT_EQ(file::read(written), "token /given path");
}

TEST(ProcessTest, DetachedStartsReportFailures)
{
    EXPECT_EQ(Process::startDetached("cutefish-no-such-program"), -1);
    EXPECT_EQ(Process::startDetached("true", {}, "/nonexistent/directory"), -1);
}

// "SigBlk:" or "SigIgn:" of /proc/self/status, as a mask.
static unsigned long long statusMask(const std::string& status, const std::string& field)
{
    const size_t at = status.find(field + ":");
    return at == std::string::npos ? ~0ULL : std::stoull(status.substr(at + field.size() + 1), nullptr, 16);
}

// A process that blocks or ignores signals for itself, as one reading them
// from a signalfd does, starts programs that can be ended with them.
TEST(ProcessTest, ProgramsStartWithNoSignalBlockedOrIgnored)
{
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGHUP);
    sigset_t previous;
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    struct sigaction ignore = {};
    ignore.sa_handler = SIG_IGN;
    struct sigaction old = {};
    sigaction(SIGINT, &ignore, &old);

    const unsigned long long term = 1ULL << (SIGTERM - 1);
    const unsigned long long hup = 1ULL << (SIGHUP - 1);
    const unsigned long long interrupt = 1ULL << (SIGINT - 1);
    EventLoop loop;
    Process process(&loop);
    ASSERT_TRUE(process.start("cat", {"/proc/self/status"}));
    ASSERT_TRUE(runUntilFinished(loop, process));
    EXPECT_EQ(statusMask(process.standardOutput(), "SigBlk") & (term | hup), 0u);
    EXPECT_EQ(statusMask(process.standardOutput(), "SigIgn") & interrupt, 0u);

    const std::string path = std::filesystem::temp_directory_path() / ("process-test-" + std::to_string(getpid()));
    ASSERT_GT(Process::startDetached("sh", {"-c", "cat /proc/self/status > " + path + ".part && mv " + path + ".part " + path}), 0);
    for (int i = 0; i < 500 && !std::filesystem::exists(path); ++i)
        usleep(10000);
    const std::string detached = file::read(path).value_or(std::string());
    std::filesystem::remove(path);
    EXPECT_EQ(statusMask(detached, "SigBlk") & (term | hup), 0u);
    EXPECT_EQ(statusMask(detached, "SigIgn") & interrupt, 0u);

    sigaction(SIGINT, &old, nullptr);
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}
