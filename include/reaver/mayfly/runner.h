/**
* Mayfly License
*
* Copyright © 2014 Michał "Griwes" Dominiak
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

#include <memory>
#include <iostream>
#include <chrono>

#include <boost/process.hpp>
#include <boost/process/initializers.hpp>
#include <boost/program_options.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/algorithm/search.hpp>

#include <reaver/thread_pool.h>

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
            runner(std::size_t threads = 1, std::size_t timeout = 60, std::string test_name = "") : _threads{ threads }, _timeout{ timeout}, _test_name{ std::move(test_name) }
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
                rep.summary(_failed, _passed, _tests);
            }

        protected:
            std::size_t _threads = 1;
            std::size_t _limit = 0;
            std::size_t _timeout = 60;

            std::string _test_name;

            std::atomic<std::uintmax_t> _tests{};
            std::atomic<std::uintmax_t> _passed{};

            std::vector<std::pair<testcase_status, std::string>> _failed;
        };

        class subprocess_runner : public runner
        {
        public:
            subprocess_runner(std::string executable, std::size_t threads = 1, std::size_t timeout = 60, std::string test_name = "") : runner{ threads, timeout, std::move(test_name) },
                _executable{ std::move(executable) }
            {
            }

            virtual void operator()(const std::vector<suite> & suites, const reporter & rep) override
            {
                for (const auto & s : suites)
                {
                    _handle_suite(s, rep);
                }
            }

        private:
            std::string _executable;

            void _handle_suite(const suite & s, const reporter & rep, std::vector<std::string> suite_stack = {})
            {
                suite_stack.push_back(s.name());
                if (boost::range::search(_test_name, boost::join(suite_stack, "/")) != _test_name.begin())
                {
                    return;
                }

                rep.suite_started(s);

                for (const auto & sub : s.suites())
                {
                    _handle_suite(sub, rep, suite_stack);
                }

                {
                    thread_pool pool(_threads);

                    for (const auto & test : s)
                    {
                        if (!_test_name.empty() && boost::join(suite_stack, "/") + "/" + test.name() != _test_name)
                        {
                            continue;
                        }

                        ++_tests;

                        pool.push([=, &rep]()
                        {
                            auto result = _run_test(test, s, rep, suite_stack);

                            if (result.status == testcase_status::passed)
                            {
                                ++_passed;
                            }

                            else
                            {
                                _failed.push_back(std::make_pair(result.status, boost::join(suite_stack, "/") + "/" + test.name()));
                            }
                        });
                    }
                }

                rep.suite_finished(s);
            }

            testcase_result _run_test(const testcase & t, const suite & s, const reporter & rep, const std::vector<std::string> & suite_stack) const
            {
                testcase_result result;
                result.name = t.name();

                if (_threads == 1)
                {
                    rep.test_started(t);
                }

                if (boost::join(suite_stack, "/") + "/" + t.name() == _test_name)
                {
                    try
                    {
                        t();
                        result.status = testcase_status::passed;
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
                }

                else
                {
                    using namespace boost::process::initializers;

                    std::vector<std::string> args{ _executable, "--test", boost::join(suite_stack, "/") + "/" + t.name(), "-r", "subprocess" };

                    boost::process::pipe p = boost::process::create_pipe();
                    std::atomic<bool> timeout_flag{ false };
                    std::atomic<bool> finished_flag{ false };
                    std::mutex m;
                    std::condition_variable cv;
                    std::thread t;

                    auto begin = std::chrono::high_resolution_clock::now();

                    {
                        boost::iostreams::file_descriptor_sink sink{ p.sink, boost::iostreams::close_handle };

                        auto child = boost::process::execute(set_args(args), inherit_env(), bind_stdout(sink), close_stdin());

                        t = std::thread{ [&]()
                        {
                            std::unique_lock<std::mutex> lock{ m };
                            if (!cv.wait_for(lock, std::chrono::seconds(_timeout), [&]() -> bool { return finished_flag; }))
                            {
                                timeout_flag = true;
                                boost::process::terminate(child);
                            }
                        }};
                    }

                    boost::iostreams::file_descriptor_source source{ p.source, boost::iostreams::close_handle };
                    boost::iostreams::stream<boost::iostreams::file_descriptor_source> is(source);

                    std::uintmax_t retval;
                    std::string message;

                    try
                    {
                        is >> retval;
                        std::getline(is, message);
                        message = message.substr(1);            // remove the leading space; thanks, b.iostreams for no .ignore()

                        try
                        {
                            finished_flag = true;
                            cv.notify_all();
                            t.join();
                        }

                        catch (...)
                        {
                        }

                        if (!is || retval > 3)
                        {
                            result.status = testcase_status::crashed;
                        }

                        else
                        {
                            result.status = static_cast<testcase_status>(retval);
                            result.description = message;
                        }
                    }

                    catch (...)
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

                    if (t.joinable())
                    {
                        t.detach();
                    }
                }

                if (_threads != 1)
                {
                    std::unique_lock<const reporter> lock(rep);
                    rep.test_started(t);
                    rep.test_finished(result);
                }

                else
                {
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

        constexpr static const char * version_string = "Reaver Project's Mayfly v0.1.1 alpha\nCopyright © 2014 Reaver Project Team\n";

        class invalid_testcase_name_format : public exception
        {
        public:
            invalid_testcase_name_format(const std::string & test_name) : exception{ reaver::logger::error }
            {
                *this << "invalid testcase name format - proper format is `suite(s)/testcase`.";
            }
        };

        inline int run(const std::vector<suite> & suites, int argc, char ** argv)
        {
            std::size_t threads = 1;
            std::size_t timeout = 60;
            std::string test_name;
            std::vector<std::string> reporters;
            std::string executable = argv[0];

            boost::program_options::variables_map variables;

            boost::program_options::options_description general("General");
            general.add_options()
                ("help,h", "print this message")
                ("version,v", "print version information");

            boost::program_options::options_description config("Configuration");
            config.add_options()
                ("tasks,j", boost::program_options::value<std::size_t>(&threads), "specify the amount of worker threads")
                ("test,t", boost::program_options::value<std::string>(&test_name), "specify the thread to run")
                ("reporter,r", boost::program_options::value<std::vector<std::string>>()->composing(), "select a reporter to use")
                ("quiet,q", "disable reporters")
                ("timeout,l", boost::program_options::value<std::size_t>(&timeout), "specify the timeout for tests (in seconds)")
                ("error,e", "only show errors and summary (controls console output)");

            boost::program_options::options_description options;
            options.add(general).add(config);

            boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(options)
                .style(boost::program_options::command_line_style::allow_short
                    | boost::program_options::command_line_style::allow_long
                    | boost::program_options::command_line_style::allow_sticky
                    | boost::program_options::command_line_style::allow_dash_for_short
                    | boost::program_options::command_line_style::long_allow_next
                    | boost::program_options::command_line_style::short_allow_next
                    | boost::program_options::command_line_style::allow_long_disguise).run(), variables);
            boost::program_options::notify(variables);

            if (variables.count("help"))
            {
                std::cout << version_string << '\n';
                std::cout << general << '\n' << config;

                return 0;
            }

            if (variables.count("version"))
            {
                std::cout << version_string;
                std::cout << "Distributed under modified zlib license.\n\n";

                std::cout << "Mayfly is the Reaver Project's free testing framework.\n";

                return 0;
            }

            if (variables.count("reporter"))
            {
                reporters = variables["reporter"].as<std::vector<std::string>>();
            }

            else if (!variables.count("quiet"))
            {
                reporters.push_back("console");
            }

            if (variables.count("error"))
            {
                reaver::logger::default_logger().set_level(reaver::logger::error);
            }

            std::vector<std::reference_wrapper<const reporter>> reps;
            for (const auto & elem : reporters)
            {
                reps.emplace_back(std::cref(*reporter_registry().at(elem)));
            }

            if (!test_name.empty() && test_name.find('/') == std::string::npos)
            {
                if (!variables.count("quiet"))
                {
                    throw invalid_testcase_name_format{ test_name };
                }

                else
                {
                    std::cout << static_cast<std::uintmax_t>(testcase_status::not_found) << std::flush;
                    return 1;
                }
            }

            auto && reporter = combine(reps);
            default_runner(std::make_unique<subprocess_runner>(executable, threads, timeout, test_name))(suites, reporter);
            default_runner().summary(reporter);

            if (default_runner().passed() == default_runner().total())
            {
                return 0;
            }

            return 1;
        }
    }}
}
