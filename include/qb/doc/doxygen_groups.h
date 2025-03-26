/**
 * @file qb/doc/doxygen_groups.h
 * @brief Definition of documentation groups
 * 
 * This file is not meant to be included in code. It only contains documentation
 * that defines the groups and subgroups for organizing the API documentation.
 * 
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2025 qb - C++ Actor Framework (cpp.actor)
 */

/**
 * @defgroup QB QB Framework
 * @brief Core components of the QB Actor Framework
 * 
 * The QB Framework is a C++ Actor Framework designed for high-performance
 * concurrent and distributed applications. It provides a comprehensive set of
 * tools for building systems based on the Actor Model paradigm.
 */

/**
 * @defgroup Core Core Actor System
 * @ingroup QB
 * @brief Fundamental components of the actor model implementation
 * 
 * The Core module provides the essential classes and structures that implement
 * the Actor Model, including actor lifecycle management, message passing, and
 * event handling mechanisms.
 */

/**
 * @defgroup Actor Actor Components
 * @ingroup Core
 * @brief Actor classes and lifecycle management
 * 
 * This group contains the classes related to actor definition, creation,
 * lifecycle management, and interaction patterns. The Actor is the fundamental
 * unit of computation in the QB framework.
 */

/**
 * @defgroup Event Event System
 * @ingroup Core
 * @brief Event and message passing system
 * 
 * The Event System provides the mechanisms for passing messages between actors,
 * handling events, and managing the asynchronous communication patterns that
 * form the basis of the Actor Model.
 */

/**
 * @defgroup IO I/O and Networking
 * @ingroup QB
 * @brief I/O operations and network communication
 * 
 * The IO module handles input/output operations, network communications,
 * and provides abstractions for dealing with various protocols and
 * communication patterns.
 */

/**
 * @defgroup Async Asynchronous I/O
 * @ingroup IO
 * @brief Event-based asynchronous I/O
 * 
 * This group contains components for non-blocking, event-driven I/O operations,
 * including file I/O, network I/O, and various asynchronous patterns.
 */

/**
 * @defgroup Transport Transport Protocols
 * @ingroup IO
 * @brief Network transport implementations
 * 
 * Transport protocols provide the low-level implementations for various network
 * communication methods, including TCP, UDP, and higher-level protocol abstractions.
 */

/**
 * @defgroup Utility Utility Libraries
 * @ingroup QB
 * @brief General-purpose utility libraries
 * 
 * The Utility module provides various helper classes, functions, and data structures
 * that are used throughout the framework for common operations.
 */

/**
 * @defgroup Container Containers
 * @ingroup Utility
 * @brief Data container structures
 * 
 * This group includes custom container implementations and wrappers that provide
 * specific performance or functionality characteristics needed by the framework.
 */

/**
 * @defgroup Memory Memory Management
 * @ingroup Utility
 * @brief Memory management and allocators
 * 
 * The Memory Management components provide custom allocators, memory pools,
 * and other tools for efficient memory usage in high-performance scenarios.
 */

/**
 * @defgroup System System Facilities
 * @ingroup QB
 * @brief System-level facilities and abstractions
 * 
 * The System module provides abstractions and utilities for interacting with
 * operating system facilities, hardware features, and low-level system resources.
 */

/**
 * @defgroup Lockfree Lock-free Data Structures
 * @ingroup System
 * @brief Lock-free concurrent data structures
 * 
 * This group contains implementations of various lock-free data structures
 * designed for high-performance concurrent access without using traditional
 * locking mechanisms.
 */

/**
 * @defgroup CPU CPU Features
 * @ingroup System
 * @brief CPU-specific optimizations and features
 * 
 * The CPU Features components provide abstractions and utilities for working with
 * specific CPU capabilities, optimizations, and hardware-specific features.
 */ 