/*
 * qb - C++ Actor Framework
 * Copyright (c) 2011-2025 qb - isndev (cpp.actor). All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*!
 * @mainpage QB Actor Framework: High-Performance C++17 Concurrent Systems
 * @image html template/static/logo.svg width=250px
 * 
 * Welcome to the official documentation for the **QB Actor Framework** – your C++17 toolkit 
 * for crafting powerful, scalable, and maintainable concurrent and distributed applications.
 *
 * QB empowers developers to build responsive, high-performance systems by elegantly integrating 
 * the **Actor Model** with a robust **Asynchronous I/O Engine**. Whether you're tackling 
 * real-time data processing, complex network services, or large-scale distributed computations, 
 * QB provides the tools and abstractions to simplify development and maximize efficiency.
 *
 * This site offers a comprehensive guide, from foundational concepts and API details to 
 * practical examples and advanced usage patterns.
 *
 * @section main_project_readme Project README
 *   For a comprehensive overview, features, a quick code example, and top-level navigation, 
 *   please start with the main **[Project README](../README.md)**.
 *
 * @section main_getting_started Getting Started Fast
 * - **New to QB?** Begin with the **[Introduction Section Overview](../readme/1_introduction/README.md)**.
 * - Jump right in: **[Step-by-Step Getting Started Guide](../readme/6_guides/getting_started.md)**.
 *
 * @section main_documentation_structure Navigating The Documentation
 *
 * The documentation is organized into the following key areas:
 *
 * - **1. Introduction **
 *      - @subpage introduction_readme "Introduction Overview"
 *      - @subpage intro_overview_md "Framework Overview"
 *      - @subpage intro_philosophy_md "Core Philosophy"
 *
 * - **2. Core Concepts **
 *      - @subpage core_concepts_readme "Core Concepts Overview"
 *      - @subpage core_concepts_actor_model_md "Actor Model"
 *      - @subpage core_concepts_event_system_md "Event System"
 *      - @subpage core_concepts_async_io_md "Async I/O Model"
 *      - @subpage core_concepts_concurrency_md "Concurrency & Parallelism"
 *
 * - **3. QB-IO Module (Asynchronous I/O & Utilities) **
 *      - @subpage qb_io_readme_md "QB-IO Overview"
 *      - @subpage qb_io_features_md "QB-IO Features"
 *      - @subpage qb_io_async_system_md "QB-IO Async Engine"
 *      - @subpage qb_io_transports_md "QB-IO Transports"
 *      - @subpage qb_io_protocols_md "QB-IO Protocols"
 *      - @subpage qb_io_ssl_transport_md "QB-IO SSL/TLS"
 *      - @subpage qb_io_utilities_md "QB-IO Utilities"
 *
 * - **4. QB-Core Module (Actor Engine) **
 *      - @subpage qb_core_readme_md "QB-Core Overview"
 *      - @subpage qb_core_features_md "QB-Core Features"
 *      - @subpage qb_core_actor_md "Mastering qb::Actor"
 *      - @subpage qb_core_messaging_md "Event Messaging"
 *      - @subpage qb_core_engine_md "Engine: Main & VirtualCore"
 *      - @subpage qb_core_patterns_md "Actor Patterns & Utilities"
 *
 * - **5. Core & IO Integration **
 *      - @subpage core_io_integration_readme_md "Core & IO Integration Overview"
 *      - @subpage core_io_async_in_actors_md "Async Ops in Actors"
 *      - @subpage core_io_network_actors_md "Network-Enabled Actors"
 *      - **Example Analyses **
 *          - @subpage core_io_example_analyses_readme "Example Analyses Overview"
 *          - @subpage example_analysis_chat_tcp_md "Example: TCP Chat"
 *          - @subpage example_analysis_dist_comp_md "Example: Distributed Computing"
 *          - @subpage example_analysis_file_monitor_md "Example: File Monitor"
 *          - @subpage example_analysis_file_processor_md "Example: File Processor"
 *          - @subpage example_analysis_msg_broker_md "Example: Message Broker"
 *
 * - **6. Developer Guides **
 *      - @subpage guides_readme "Developer Guides Overview"
 *      - @subpage guides_getting_started_md "Getting Started"
 *      - @subpage guides_patterns_cookbook_md "Design Patterns Cookbook"
 *      - @subpage guides_advanced_usage_md "Advanced Usage"
 *      - @subpage guides_performance_tuning_md "Performance Tuning"
 *      - @subpage guides_error_handling_md "Error Handling"
 *      - @subpage guides_resource_management_md "Resource Management"
 *
 * - **7. Reference **
 *      - @subpage reference_readme "Reference Overview"
 *      - @subpage ref_api_overview_md "API Overview"
 *      - @subpage ref_building_md "Building QB"
 *      - @subpage ref_testing_md "Testing QB"
 *      - @subpage ref_faq_md "FAQ"
 *      - @subpage ref_glossary_md "Glossary"
 *      - @subpage ref_lockfree_primitives_md "Lock-Free Primitives"
 *
 * @section main_build_status Build Status
 *   |              | linux | Windows | Coverage |
 *   |:------------:|:-----:|:-------:|:--------:|
 *   |    master    | ![Build Status](https://travis-ci.org/isndev/qb.svg?branch=master) | ![Build status](https://ci.appveyor.com/api/projects/status/aern7ygl63wa3c9b/branch/master?svg=true) | ![Codecov branch](https://img.shields.io/codecov/c/github/isndev/qb/master.svg) |
 *   |    develop   | ![Build Status](https://travis-ci.org/isndev/qb.svg?branch=develop) | ![Build status](https://ci.appveyor.com/api/projects/status/aern7ygl63wa3c9b/branch/develop?svg=true) | ![Codecov branch](https://img.shields.io/codecov/c/github/isndev/qb/develop.svg) |
 *   | experimental | ![Build Status](https://travis-ci.org/isndev/qb.svg?branch=experimental) | ![Build status](https://ci.appveyor.com/api/projects/status/aern7ygl63wa3c9b/branch/experimental?svg=true) | ![Codecov branch](https://img.shields.io/codecov/c/github/isndev/qb/experimental.svg) |
 *
 * #### License
 *   Apache Version 2
 */ 