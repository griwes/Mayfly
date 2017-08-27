/**
* Mayfly License
*
* Copyright © 2014-2015, 2017 Michał "Griwes" Dominiak
*
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
*
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
*
* 1. The origin of this software must not be misrepresented; you must not
*    claim that you wrote the original software. If you use this software
*    in a product, an acknowledgment in the product documentation is required.
* 2. Altered source versions must be plainly marked as such, and must not be
*    misrepresented as being the original software.
* 3. This notice may not be removed or altered from any source distribution.
*
**/

#pragma once

#include <vector>
#include <memory>
#include <iostream>
#include <chrono>

#include <boost/process.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm/search.hpp>
#include <boost/optional.hpp>

#include <reaver/thread_pool.h>
#include <reaver/configuration/options.h>

#include "reporter.h"
#include "testcase.h"
#include "suite.h"
#include "console.h"
#include "subprocess.h"

namespace reaver
{
    namespace mayfly { inline namespace _v1
    {
        class runner
        {
        public:
            runner(std::size_t threads = 1, std::size_t timeout = 60, boost::optional<std::string> test_name = {}) : _threads{ threads }, _timeout{ timeout}, _test_name{ std::move(test_name) }
            {
            }

            virtual ~runner() {}

            virtual void operator()(const std::vector<suite> & suites, const reporter & rep) = 0;

            std::size_t total() const
            {
                return _tests;
            }

            std::size_t passed() const
            {
                return _passed;
            }

            auto summary(const reporter & rep) const
            {
                rep.summary({ _failed, _passed, _tests, _last_actual_time });
            }

        protected:
            std::size_t _threads = 1;
            std::size_t _limit = 0;
            std::size_t _timeout = 60;

            boost::optional<std::string> _test_name;

            std::atomic<std::uintmax_t> _tests{};
            std::atomic<std::uintmax_t> _passed{};

            mutable std::mutex _failed_mtx;
            std::vector<std::pair<testcase_status, std::string>> _failed;
            std::chrono::milliseconds _last_actual_time{ 0 };
        };

        class subprocess_runner : public runner
        {
        public:
            subprocess_runner(std::string executable, std::size_t threads = 1, std::size_t timeout = 60, boost::optional<std::string> test_name = {}) : runner{ threads, timeout, std::move(test_name) },
                _executable{ std::move(executable) }
            {
            }

            virtual void operator()(const std::vector<suite> & suites, const reporter & rep) override
            {
                if (_test_name)
                {
                    std::vector<std::string> suite_names;
                    boost::algorithm::split(suite_names, *_test_name, boost::is_any_of("/"));
                    testcase_result result;
                    result.status = testcase_status::passed;
                    result.name = std::move(suite_names.back());
                    suite_names.pop_back();

                    boost::optional<const testcase &> t;

                    try
                    {
                        auto ref = std::ref(*boost::range::find_if(suites, [&](const suite & s){ return s.name() == suite_names.front(); }));
                        suite_names.erase(suite_names.begin());
                        for (auto && s : suite_names)
                        {
                            ref = std::ref(ref.get()[s]);
                        }
                        auto it = std::find_if(ref.get().begin(), ref.get().end(), [&](const testcase & tc){ return tc.name() == result.name; });
                        if (it == ref.get().end())
                        {
                            return;
                        }
                        t = *it;
                    }

                    catch (...)
                    {
                        return;
                    }

                    rep.test_started(*t);
                    auto begin = std::chrono::high_resolution_clock::now();

                    try
                    {
                        (*t)();
                    }

                    catch (reaver::exception & e)
                    {
                        std::ostringstream str;

                        {
                            reaver::logger::logger l{};
                            l.add_stream(str);
                            e.print(l);
                        }

                        result.status = testcase_status::failed;
                        result.description = str.str();
                    }

                    catch (std::exception & e)
                    {
                        result.status = testcase_status::failed;
                        result.description = e.what();
                    }

                    if (result.status == testcase_status::passed)
                    {
                        ++_passed;
                    }
                    ++_tests;

                    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - begin);
                    _last_actual_time = result.duration;
                    rep.test_finished(result);

                    return;
                }

                auto start = std::chrono::high_resolution_clock::now();
                {
                    std::unique_ptr<thread_pool> pool = _threads > 1 ? std::make_unique<thread_pool>(_threads) : nullptr;
                    for (const auto & s : suites)
                    {
                        _handle_suite(pool.get(), s, rep);
                    }
                }
                _last_actual_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start);
            }

        private:
            std::string _executable;

            void _handle_suite(thread_pool * pool, const suite & s, const reporter & rep, std::vector<std::reference_wrapper<const suite>> suite_stack = {})
            {
                suite_stack.push_back(s);
                if (_test_name && boost::range::search(*_test_name, boost::join(fmap(suite_stack, [](auto && s){ return s.get().name(); }), "/")) != _test_name->begin())
                {
                    return;
                }

                if (!pool)
                {
                    rep.suite_started(suite_stack.back().get());
                }

                for (const auto & sub : s.suites())
                {
                    _handle_suite(pool, sub, rep, suite_stack);
                }

                {
                    for (const auto & test : s)
                    {
                        if (_test_name && boost::join(fmap(suite_stack, [](auto && s){ return s.get().name(); }), "/") + "/" + test.name() != _test_name)
                        {
                            continue;
                        }

                        ++_tests;

                        auto callback = [=, &rep]()
                        {
                            auto result = _run_test(test, s, rep, suite_stack);

                            if (result.status == testcase_status::passed)
                            {
                                ++_passed;
                            }

                            else
                            {
                                std::lock_guard<std::mutex> lock{ _failed_mtx };
                                _failed.push_back(std::make_pair(result.status, boost::join(fmap(suite_stack, [](auto && s){ return s.get().name(); }), "/") + "/" + test.name()));
                            }
                        };

                        if (pool)
                        {
                            pool->push(callback);
                        }

                        else
                        {
                            callback();
                        }
                    }
                }

                if (!pool)
                {
                    rep.suite_finished(suite_stack.back().get());
                }
            }

            testcase_result _run_test(const testcase & t, const suite & s, const reporter & rep, std::vector<std::reference_wrapper<const suite>> suite_stack) const
            {
                testcase_result result;
                result.name = t.name();
                result.status = testcase_status::passed;

                std::vector<std::string> output;

                if (_threads == 1)
                {
                    rep.test_started(t);
                }

                std::vector<std::string> args{ _executable, "--test", boost::join(fmap(suite_stack, [](auto && s){ return s.get().name(); }), "/") + "/" + t.name(), "-r", "subprocess" };

                boost::process::ipstream out;

                auto begin = std::chrono::high_resolution_clock::now();
                bool timeout_flag = false;

                boost::process::child child(args, boost::process::std_out > out);
                std::error_code ec;
                if (!child.wait_for(std::chrono::seconds{ _timeout }, ec))
                {
                    timeout_flag = true;
                    child.terminate(ec);
                }

                std::string message;

                enum { not_started, started, finished, exited } state = not_started;

                while (std::getline(out, message))
                {
                    if (message.substr(0, 2) == "{{")
                    {
                        if (message == "{{started}}")
                        {
                            assert(state == not_started);
                            state = started;
                        }

                        else if (message == "{{finished}}")
                        {
                            assert(state == started);
                            state = finished;
                        }

                        else if (message == "{{exit}}")
                        {
                            assert(state == finished);
                            state = exited;
                        }

                        else if (message.substr(0, 8) == "{{failed")
                        {
                            assert(state == started);
                            result.status = testcase_status::failed;
                            result.description = message.substr(9, message.length() - 11);
                        }

                        else if (message == "{{error unexpected test status}}")
                        {
                            throw unexpected_result{};
                        }

                        else if (message == "{{error not found}}")
                        {
                            result.status = testcase_status::not_found;
                        }
                    }

                    else
                    {
                        output.push_back(std::move(message));
                    }
                }

                if (state != exited)
                {
                    if (timeout_flag)
                    {
                        result.status = testcase_status::timed_out;
                    }

                    else
                    {
                        result.status = testcase_status::crashed;
                    }
                }

                auto duration = std::chrono::high_resolution_clock::now() - begin;
                result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(duration);

                if (_threads != 1)
                {
                    std::unique_lock<const reporter> lock(rep);
                    for (auto && s : suite_stack)
                    {
                        rep.suite_started(s);
                    }
                    rep.test_started(t);
                    for (auto && message : output)
                    {
                        logger::dlog() << message;
                    }
                    rep.test_finished(result);
                    for (auto && s : suite_stack)
                    {
                        rep.suite_finished(s);
                    }
                }

                else
                {
                    for (auto && message : output)
                    {
                        logger::dlog() << message;
                    }
                    rep.test_finished(result);
                }

                return result;
            }
        };

        class invalid_default_runner_initialization : public exception
        {
        public:
            invalid_default_runner_initialization() : exception{ logger::crash }
            {
                *this << "attempted to initialize Mayfly's default runner with a null value.";
            }
        };

        inline runner & default_runner(std::unique_ptr<runner> new_default = nullptr)
        {
            static std::unique_ptr<runner> default_runner = [&]()
            {
                if (new_default)
                {
                    return std::move(new_default);
                }

                throw invalid_default_runner_initialization{};
            }();

            if (new_default)
            {
                default_runner = std::move(new_default);
            }

            return *default_runner;
        }

        constexpr static const char * version_string = "Reaver Project's Mayfly v0.1.2 alpha\nCopyright © 2014, 2017 Reaver Project Team\n";

        class invalid_testcase_name_format : public exception
        {
        public:
            invalid_testcase_name_format(const std::string & test_name) : exception{ reaver::logger::error }
            {
                *this << "invalid testcase name format - proper format is `suite(s)/testcase`.";
            }
        };

        namespace options
        {
            new_opt_desc(help, void, "help,h", "print this message");
            new_opt_desc(version, void, "version,v", "print version information");

            new_opt_ext(tasks, std::size_t, opt_name_desc("tasks,j", "specify the amount of worker threads"); static constexpr type default_value = 1; );
            new_opt_desc(test, boost::optional<std::string>, "test,t", "specify the test to run");
            new_opt_desc(reporter, std::vector<std::string>, "reporter,r", "select reporters to use");
            new_opt_desc(quiet, void, "quiet,q", "disable reporters");
            new_opt_ext(timeout, std::size_t, opt_name_desc("timeout,l", "specify the timeout for tests (in seconds)"); static constexpr type default_value = 10; );
            new_opt_desc(error, void, "error,e", "only show errors and summary (controls console output)");
        }

        inline int run(const std::vector<suite> & suites, int argc, char ** argv)
        {
            std::string executable = argv[0];

            boost::program_options::variables_map variables;

            boost::program_options::options_description general("General");
            general.add_options()
                ("help,h", "print this message")
                ("version,v", "print version information");

            boost::program_options::options_description config("Configuration");
            config.add_options()
                ("tasks,j", boost::program_options::value<std::size_t>(), "specify the amount of worker threads")
                ("test,t", boost::program_options::value<std::string>(), "specify the thread to run")
                ("reporter,r", boost::program_options::value<std::vector<std::string>>()->composing(), "select a reporter to use")
                ("quiet,q", "disable reporters")
                ("timeout,l", boost::program_options::value<std::size_t>(), "specify the timeout for tests (in seconds)")
                ("error,e", "only show errors and summary (controls console output)");

            boost::program_options::options_description options;
            options.add(general).add(config);

            auto parsed = reaver::options::parse_argv(argc, argv, tpl::vector<options::help, options::version, options::tasks, options::test, options::reporter, options::quiet, options::timeout, options::error>{});

            if (parsed.get<options::help>())
            {
                std::cout << version_string << '\n';
                std::cout << general << '\n' << config;

                return 0;
            }

            if (parsed.get<options::version>())
            {
                std::cout << version_string;
                std::cout << "Distributed under modified zlib license.\n\n";

                std::cout << "Mayfly is the Reaver Project's free testing framework.\n";

                return 0;
            }

            auto reporters = parsed.get<options::reporter>();
            if (reporters.empty() && !parsed.get<options::quiet>())
            {
                reporters.push_back("console");
            }

            if (parsed.get<options::error>())
            {
                reaver::logger::default_logger().set_level(reaver::logger::error);
            }

            std::vector<std::reference_wrapper<const reporter>> reps;
            for (const auto & elem : reporters)
            {
                reps.emplace_back(std::cref(*reporter_registry().at(elem)));
            }

            auto test_name = parsed.get<options::test>();
            if (test_name && test_name->find('/') == std::string::npos)
            {
                if (!parsed.get<options::quiet>())
                {
                    throw invalid_testcase_name_format{ *test_name };
                }

                else
                {
                    std::cout << static_cast<std::uintmax_t>(testcase_status::not_found) << std::flush;
                    return 1;
                }
            }

            auto && reporter = combine(reps);
            default_runner(std::make_unique<subprocess_runner>(executable, parsed.get<options::tasks>(), parsed.get<options::timeout>(), test_name))(suites, reporter);
            default_runner().summary(reporter);

            if (default_runner().passed() == default_runner().total())
            {
                return 0;
            }

            return 1;
        }
    }}
}
