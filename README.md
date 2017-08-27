# Mayfly

[![Join the chat at https://gitter.im/reaver-project/Mayfly](https://badges.gitter.im/reaver-project/Mayfly.svg)](https://gitter.im/reaver-project/Mayfly?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

Mayfly is the Reaver Project's testing framework. It's meant to be lightweight and
benefit from as many modern C++ features as possible. Primary goals for Mayfly are
to be fast, generate readable outputs (and TeamCity-ready results) and later get
closely integrated with Despayre, the Reaver Project's build system.

## Supported compilers

Currently supported compilers include GCC 6.1+ and Clang 3.9+, both with libstdc++
and libc++, and with Boost 1.64.0+. 

CI status can be checked on [Reaver Project's TeamCity server](http://ci.reaver-project.org/viewType.html?buildTypeId=mayfly_Tests&guest=1).
