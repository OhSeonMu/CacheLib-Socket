/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/io/async/EventBase.h>
#include <folly/logging/LoggerDB.h>
#include <gflags/gflags.h>
//mhkim
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>

#include <memory>
#include <thread>

#include "cachelib/cachebench/runner/Runner.h"
#include "cachelib/cachebench/runner/Stressor.h"
#include "cachelib/common/Utils.h"

#ifdef CACHEBENCH_FB_ENV
#include "cachelib/cachebench/facebook/FbDep.h"
#include "cachelib/cachebench/facebook/fb303/FB303ThriftServer.h"
#include "cachelib/facebook/odsl_exporter/OdslExporter.h"
#include "common/init/Init.h"
#else
#include <folly/init/Init.h>
#include <gflags/gflags.h>
#endif

#ifdef CACHEBENCH_FB_ENV
DEFINE_bool(export_to_ods, false, "Upload cachelib stats to ODS");
DEFINE_int32(fb303_port,
             0,
             "Port for cachebench fb303 service. If 0, do not export to fb303. "
             "If valid, this will disable ODSL export.");
#endif
DEFINE_string(json_test_config,
              "",
              "path to test config. If empty, use default setting");
DEFINE_uint64(
    progress,
    60,
    "if set, prints progress every X seconds as configured, 0 to disable");
DEFINE_string(progress_stats_file,
              "",
              "Print detailed stats at each progress interval to this file");
DEFINE_int32(timeout_seconds,
             0,
             "Maximum allowed seconds for running test. 0 means no timeout");

struct sigaction act;
std::unique_ptr<facebook::cachelib::cachebench::Runner> runnerInstance;
std::unique_ptr<std::thread> stopperThread;

void sigint_handler(int sig_num) {
  switch (sig_num) {
  case SIGINT:
  case SIGTERM: {
    if (runnerInstance) {
      runnerInstance->abort();
    }
    break;
  }
  }
}

void setupSignalHandler() {
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = &sigint_handler;
  act.sa_flags = SA_RESETHAND;
  if (sigaction(SIGINT, &act, nullptr) == -1 ||
      sigaction(SIGTERM, &act, nullptr) == -1) {
    std::cout << "Failed to register SIGINT&SIGTERM handler" << std::endl;
    std::exit(1);
  }
}

void setupTimeoutHandler() {
  if (FLAGS_timeout_seconds > 0) {
    stopperThread = std::make_unique<std::thread>([] {
      folly::EventBase eb;
      eb.runAfterDelay(
          [&eb]() {
            XLOGF(INFO,
                  "Stopping due to timeout {} seconds",
                  FLAGS_timeout_seconds);
            if (runnerInstance) {
              runnerInstance->abort();
            }
            eb.terminateLoopSoon();
          },
          FLAGS_timeout_seconds * 1000);
      eb.loopForever();
      // We give another few seconds for the graceful shutdown to complete
      eb.runAfterDelay([]() { XCHECK(false); }, 30 * 1000);
      eb.loopForever();
    });
    stopperThread->detach();
  }
}

bool checkArgsValidity() {
  if (FLAGS_json_test_config.empty() ||
      !facebook::cachelib::util::pathExists(FLAGS_json_test_config)) {
    std::cout << "Invalid config file: " << FLAGS_json_test_config
              << ". pass a valid --json_test_config for cachebench."
              << std::endl;
    return false;
  }

  return true;
}

int GetTimeout() {
	int socket_fd;
	int opt = 1;
	struct sockaddr_in address;
	int port = 9709;
	int num_timeout = 0;
	int new_socket;
	int addrlen = sizeof(address);
	char buffer[1024] = {0};

	if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
		std::cerr << "Socket creation error\n";
		return 0;
	}

	if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
		std::cerr << "Setsockopt error" << std::endl;
		return 0;
	}
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);
	if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
		std::cerr << "Bind failed" << std::endl;
	    return 0;
	}

	if (listen(socket_fd, 3) < 0) {
		std::cerr << "Listen error" << std::endl;
	    return 0;
	}

	if ((new_socket = accept(socket_fd, (struct sockaddr *) &address, (socklen_t *) &addrlen)) < 0) {
		std::cerr << "Accept error" << std::endl;
		return 0;
	}

	read(new_socket, buffer, 1024);
	num_timeout = atoi(buffer);

	close(new_socket);
	close(socket_fd);

	return num_timeout;
}

void SendStatus(facebook::cachelib::cachebench::ClientData cd) {
    struct sockaddr_in serv_addr;
    int sock = 0;
    int port = 9710;
    char ip[100] = "127.0.0.1";
    std::string message = "P99,M_Throughput\n" +
								 std::to_string(cd.p99) + "," +
								 std::to_string(cd.M_Throughput);

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Socket creation error" << std::endl;
        return;
    }   
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address / Address not supported" << std::endl;
        return;
    }   
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection Failed" << std::endl;
        return;
    }   

    send(sock, message.c_str(), message.length(), 0); 

    close(sock);
}

int main(int argc, char** argv) {
  using namespace facebook::cachelib;
  using namespace facebook::cachelib::cachebench;

#ifdef CACHEBENCH_FB_ENV
  facebook::initFacebook(&argc, &argv);
  if (!checkArgsValidity()) {
    return 1;
  }

  CacheBenchConfig config(FLAGS_json_test_config,
                          customizeCacheConfigForFacebook,
                          customizeStressorConfigForFacebook);
  std::unique_ptr<util::OdslExporter> odslExporter_;
  std::unique_ptr<FB303ThriftService> fb303_;
  if (FLAGS_fb303_port == 0 && FLAGS_export_to_ods) {
    odslExporter_ = std::make_unique<util::OdslExporter>(kCachebenchCacheName);
  } else if (FLAGS_fb303_port > 0) {
    fb303_ = std::make_unique<FB303ThriftService>(FLAGS_fb303_port);
  }
  std::cout << "Welcome to FB-internal version of cachebench" << std::endl;
#else
  folly::init(&argc, &argv, true);
  if (!checkArgsValidity()) {
    return 1;
  }

  CacheBenchConfig config(FLAGS_json_test_config);
  std::cout << "Welcome to OSS version of cachebench" << std::endl;
#endif

  try {
    runnerInstance =
        std::make_unique<facebook::cachelib::cachebench::Runner>(config);
    setupSignalHandler();

	while (true) {
		facebook::cachelib::cachebench::ClientData cd;

        FLAGS_timeout_seconds = GetTimeout();
		if (FLAGS_timeout_seconds == 0)
			return 0;

		setupTimeoutHandler();

		runnerInstance->run(std::chrono::seconds(FLAGS_progress),
                               FLAGS_progress_stats_file, &cd);

		SendStatus(cd);
	}
  } catch (const std::exception& e) {
    std::cout << "Invalid configuration. Exception: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
