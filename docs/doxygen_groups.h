/**
 * @file doxygen_groups.h
 * @brief Definition of documentation groups for the QB Actor Framework.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - C++ Actor Framework (cpp.actor)
 */

// Top-Level Group
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup QB QB Actor Framework
 * @brief The QB Actor Framework is a C++17 library for building high-performance,
 * concurrent, and distributed systems based on the Actor Model.
 *
 * It combines an efficient actor engine with a robust asynchronous I/O library
 * to simplify complex application development.
 */

// Core Actor System Modules
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup Core Core Actor System
 * @ingroup QB
 * @brief Fundamental components implementing the Actor Model.
 *
 * This module includes the actor base class, event system, engine controller,
 * virtual cores for scheduling, and actor communication primitives.
 */

/**
 * @defgroup Actor Actor Components
 * @ingroup Core
 * @brief Defines actors, their identification, and lifecycle management.
 *
 * Includes \`qb::Actor\`, \`qb::ActorId\`, \`qb::ServiceActor\`, and related concepts
 * for creating and managing concurrent entities.
 */

/**
 * @defgroup EventCore Core Event System
 * @ingroup Core
 * @brief Base event types and core system events for actors.
 *
 * Defines \`qb::Event\` and essential system events like \`qb::KillEvent\`.
 * This group is for actor-level events.
 */

/**
 * @defgroup Event Event System (General)
 * @ingroup QB
 * @brief General event concepts and system-level event handling including I/O events.
 *
 * Covers the broader event mechanisms in QB, encompassing both actor events
 * and asynchronous I/O events from the IO module.
 */

/**
 * @defgroup Engine Engine & Scheduling
 * @ingroup Core
 * @brief Manages actor execution, virtual cores, and system lifecycle.
 *
 * Contains \`qb::Main\` for engine control, \`qb::VirtualCore\` for actor execution,
 * and \`qb::CoreSet\` for CPU affinity.
 */

/**
 * @defgroup Callback Callback System
 * @ingroup Core
 * @brief Support for periodic callbacks within actors.
 *
 * Includes the \`qb::ICallback\` interface.
 */

/**
 * @defgroup PipeCore Core Communication Channels
 * @ingroup Core
 * @brief Primitives for direct actor-to-actor communication channels.
 *
 * Focuses on the \`qb::Pipe\` class used for optimized event sending.
 */

// IO System Modules
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup IO IO System
 * @ingroup QB
 * @brief Asynchronous I/O operations, networking, and related utilities.
 *
 * Provides non-blocking I/O for TCP, UDP, SSL, files, along with protocols,
 * cryptographic functions, and compression.
 */

/**
 * @defgroup Async Asynchronous System
 * @ingroup IO
 * @brief Core mechanisms for event-driven asynchronous programming.
 *
 * Includes the event listener (\`qb::io::async::listener\`), base async I/O classes
 * (\`qb::io::async::io\`), and timed callbacks (\`qb::io::async::callback\`).
 */

/**
 * @defgroup AsyncEvent Asynchronous I/O Events
 * @ingroup Async
 * @brief Specific event types for asynchronous I/O operations.
 *
 * Such as \`qb::io::async::event::disconnected\`, \`qb::io::async::event::timer\`, etc.
 * These are distinct from actor system events in EventCore.
 * @see EventCore
 */

/**
 * @defgroup Networking Networking Utilities
 * @ingroup IO
 * @brief Socket wrappers, endpoint representation, and URI parsing.
 *
 * Contains \`qb::io::socket\`, \`qb::io::endpoint\`, and \`qb::io::uri\`.
 */

/**
 * @defgroup TCP TCP Communication
 * @ingroup Networking
 * @brief Components for TCP-based network communication.
 *
 * Includes \`qb::io::tcp::socket\` and \`qb::io::tcp::listener\`.
 */

/**
 * @defgroup UDP UDP Communication
 * @ingroup Networking
 * @brief Components for UDP-based network communication.
 *
 * Includes \`qb::io::udp::socket\`.
 */

/**
 * @defgroup SSL Secure Sockets Layer (SSL/TLS)
 * @ingroup Networking
 * @brief Components for secure, encrypted TCP communication (requires OpenSSL).
 *
 * Includes \`qb::io::tcp::ssl::socket\` and \`qb::io::tcp::ssl::listener\`.
 */

/**
 * @defgroup Transport Transport Layer
 * @ingroup IO
 * @brief Abstractions over network sockets and file operations for stream-based I/O.
 *
 * Contains classes like \`qb::io::transport::tcp\`, \`qb::io::transport::udp\`,
 * \`qb::io::transport::stcp\`.
 */

/**
 * @defgroup Protocol Protocol Handling
 * @ingroup IO
 * @brief Message framing and parsing implementations.
 *
 * Defines \`qb::io::async::AProtocol\` and built-in protocols like
 * text-based, binary, and JSON.
 */

/**
 * @defgroup FileSystem File System Operations
 * @ingroup IO
 * @brief Components for interacting with the local file system.
 *
 * Includes synchronous file operations (\`qb::io::sys::file\`) and asynchronous
 * file watching (\`qb::io::async::file_watcher\`).
 */

/**
 * @defgroup Crypto Cryptographic Utilities
 * @ingroup IO
 * @brief Hashing, encryption, and JWT functionalities (requires OpenSSL).
 *
 * Contains \`qb::crypto\` and \`qb::jwt\`.
 */

/**
 * @defgroup Compression Compression Utilities
 * @ingroup IO
 * @brief Data compression and decompression (requires Zlib).
 *
 * Contains \`qb::compression\`.
 */

/**
 * @defgroup JSON JSON Utilities
 * @ingroup IO
 * @brief JSON parsing, manipulation, and serialization utilities.
 *
 * Provides \`qb::json\`, \`qb::jsonb\`, and integration with the nlohmann/json library.
 */

// System-Level Utilities
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup System System-Level Utilities
 * @ingroup QB
 * @brief Low-level system interactions and information.
 */

/**
 * @defgroup LockFree Lock-Free Primitives
 * @ingroup System
 * @brief Concurrent data structures without traditional locks.
 *
 * Includes spinlocks, MPSC/SPSC queues.
 */

/**
 * @defgroup SystemInfo System Information
 * @ingroup System
 * @brief Utilities for querying CPU and system properties.
 *
 * Contains \`qb::CPU\` and \`qb::endian\`.
 */

/**
 * @defgroup Time Time Utilities
 * @ingroup System
 * @brief High-precision timestamp and duration classes.
 *
 * Contains \`qb::Timestamp\` and \`qb::Duration\`.
 */

// General Utilities
//--------------------------------------------------------------------------------------------------
/**
 * @defgroup Utility General Utilities
 * @ingroup QB
 * @brief General-purpose helper classes and functions.
 */

/**
 * @defgroup Container Containers & Allocators
 * @ingroup Utility
 * @brief Custom containers and memory allocators for performance.
 *
 * Includes \`qb::allocator::pipe\`, \`qb::string\`, and optimized hash maps/sets.
 */

/**
 * @defgroup TypeTraits Type Traits & Metaprogramming
 * @ingroup Utility
 * @brief Advanced type traits and metaprogramming helpers.
 *
 * Contains utilities from \`qb/utility/type_traits.h\`.
 */

/**
 * @defgroup MiscUtils Miscellaneous Utilities
 * @ingroup Utility
 * @brief Other small helper utilities.
 *
 * Includes \`qb::nocopy\`, \`qb::functional\`, branch prediction hints, etc.
 */ 