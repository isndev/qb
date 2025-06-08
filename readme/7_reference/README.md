@page reference_readme QB Framework: Reference Documentation
@brief Quick access to API overviews, build instructions, testing guides, FAQs, and other reference materials for the QB Actor Framework.

# QB Framework: Reference Documentation

This section serves as a central hub for reference materials related to the QB Actor Framework. Here you'll find quick API lookups, detailed build and testing instructions, answers to frequently asked questions, a glossary of terms, and information on some of the underlying primitives used by the framework.

These documents are intended to provide specific, factual information and quick answers rather than narrative guides or conceptual explanations (which are found in earlier sections).

## Reference Topics:

*   **[QB Framework: Detailed API Overview](./api_overview.md)**
    *   A detailed mapping of key classes from `qb-core` and `qb-io`, their essential public methods, and core functionalities. Use this as a quick way to find relevant APIs.

*   **[Reference: Building the QB Actor Framework](./building.md)**
    *   Comprehensive guide to building the QB Actor Framework from source using CMake, including prerequisites, standard build steps, key CMake options, build targets, and platform-specific notes.

*   **[Reference: Testing the QB Actor Framework](./testing.md)**
    *   Information on how to build, run, and write unit and system tests for the QB framework using Google Test and CTest.

*   **[QB Actor Framework: Frequently Asked Questions (FAQ)](./faq.md)**
    *   Quick answers to common questions about using and understanding the QB Actor Framework, covering topics from thread safety to actor discovery and performance.

*   **[QB Actor Framework: Glossary of Terms](./glossary.md)**
    *   Definitions of key terms, classes, and concepts used throughout the QB Actor Framework documentation and codebase.

*   **[QB Framework: Lock-Free Primitives Reference](./lockfree_primitives.md)**
    *   An overview of the lock-free data structures (SpinLock, SPSC & MPSC Queues) used internally by QB for high-performance concurrency, primarily for informational purposes.

## How to Use This Section

*   Use the **API Overview** as a quick lookup for primary classes and methods.
*   Refer to **Building** and **Testing** guides for environment setup and development workflows.
*   Check the **FAQ** for answers to common questions before diving deeper into other documentation.
*   Consult the **Glossary** to clarify QB-specific terminology.

This reference section is designed to be a practical resource for developers actively working with or learning the QB Actor Framework.

**(Return to:** [Main Page](../../docs/mainpage.h) or explore the [Developer Guides](../6_guides/README.md) for more practical application patterns.**) 