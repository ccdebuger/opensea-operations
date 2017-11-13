# opensea-operations
Cross platform library containing set of useful operations for storage devices.

Overview 
--------
The opensea-operations library has common set of operations defined as functions
that allow command line and GUI utilities to send a collection of standard 
ATA, SCSI & NVMe commands to storage devices connected through SATA, SAS, PCIe 
or USB interfaces, including support for SATA devices attached to SAS HBAs.

The operations layer encapsulates the common usecases to be performed on storage
devices and exposes a standard API to perform those operations. 
With the help of opensea-transport and opensea-common libraries this API allows
users to write only application layer code & the library takes care of the 
OS and platform level details. 

Source
------

Building
--------
opensea-operations depends on the opensea-common & opernsea-transport library 
and the three should be cloned to the same folder for Makefiles to build
the libraries.

All Makefile and Visual Studio project & solution files are part of Make folder.

The following will build the debug version of the library by default.

cd Make/gcc
make 

To build under Microsoft Windows, open the correspoinding 
Visual Studio Solution files for VS 2013 or 2015

Documentation
-------------
Header files & functions have doxygen documentation. 

Platforms
---------
Under Linux this libraries can be built on the following platforms using 
a cross platform compiler: 

        aarch64
        alpha 
        arm 
        armhf 
        hppa 
        m68k 
        mips 
        mips64 
        mips64el
        mipsel 
        powerpc 
        powerpc64 
        powerpc64le
        s390x 
        sh4 
        x86 
        x86_64 
        
This project can be build under Windows Visual Studio 2013 & 2015 solution
files for x86 and x64 targets. 