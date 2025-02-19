OpenVVC
=======
An open-source VVC decoder library licensed under LGPLv2.1. OpenVVC is still under development.

| CI | master | dev |
|---|---|---|
| Travis CI | [![Build Status](https://travis-ci.com/OpenVVC/OpenVVC.svg?branch=master)](https://travis-ci.com/OpenVVC/OpenVVC)  |  [![Build Status](https://travis-ci.com/OpenVVC/OpenVVC.svg?branch=dev)](https://travis-ci.com/OpenVVC/OpenVVC) |
| Gitlab CI | [![pipeline status](https://gitlab.com/openvvc/openvvc/badges/master/pipeline.svg)](https://gitlab.com/openvvc/openvvc/-/commits/master) |  [![pipeline status](https://gitlab.com/openvvc/openvvc/badges/dev/pipeline.svg)](https://gitlab.com/openvvc/openvvc/-/commits/dev) |



## Table of Contents
- [Changelog](#changelog)
- [Using OpenVVC](#using-openvvc-test-program)
  - [Example](#example)
  - [Parameters](#parameters)
- [Compiling OpenVVC](#compiling-openvvc)
  - [Minimal Compilation Steps](#minimal-compilation-steps)
  - [Configure Parameters](#configure-parameters)

## Changelog
### v0.3.0
* Support the all intra, low delay and random access configuration with JVET CTCs.
* Support frame and tile parallelism.
* Support multiple build configuration.

## Using OpenVVC Test Program

### Example:

This library includes a simple executable to test the library. The executable can used as follows:

    dectest -i <file>.266 -o out.yuv

### Parameters:

```
usage: dectest [options]
options:
	-h, --help				                         Show this message.
	-v, --version				                       Show version information.
	-l <level>, --log-level=<level>		         Define the level of verbosity. Value between 0 and 6. (Default: 2)
	-i <file>, --infile=<file>		             Path to the file to be decoded (Default: test.266).
	-o <file>, --outfile=<file>		             Path to the output file (Default: test.yuv).
	-f <nbthreads>, --framethr=<nbthreads>		 Number of simultaneous frames decoded (Default: 1).
	-e <nbthreads>, --entrythr=<nbthreads>		 Number of simultaneous entries decoded per frame (Default: 1).
```

## Compiling OpenVVC

### Minimal Compilation Steps:
```
./configure
cd build
make
```
To test the library, you can perform a `make test`.

### Configure Parameters:
```
Usage: configure [options]
Options: [defaults in brackets after descriptions]

Options:
  --prefix=PREFIX          install in PREFIX [/usr/local]
  --libdir=LIBDIR          install library in LIBDIR [PREFIX/lib]
  --includedir=DIR         install includes in DIR [PREFIX/include]
  --pkgconfigdir=DIR       install pkg-config files in DIR [LIBDIR/pkgconfig]
  --build-dir=DIR          build objects in DIR [./build]
  --src-dir=DIR            build objects in DIR [.]

  --help                   print this message


  --disable-log            disable log reportings [no]
  --disable-sse            disable sse optimizations [no]
  --disable-neon           disable neon optimizations [no]
  --disable-static         do not build static libraries [no]
  --enable-shared          build shared libraries [no]

  --enable-slhdr           build slhdr feature [no]

  --cc=CC                  select compiler [gcc]
  --arch=ARCH              select architecture [x86_64]
  --target-os=OS           compiler targets OS [Linux]

  --cross-prefix=PREFIX    use PREFIX for compilation tools []
  --enable-cross-compile   assume a cross-compiler is used

  --cflags=CFLAGS          use CFLAGS as compiler flags [-Wall -O3]
  --release                use specific release compiler flags [CFLAGS -O3 -Wall]
  --debug                  use specific debug compiler cflags [CFLAGS -O0 -g -Wall --pedantic]

  --teststreams-dir=DIR    read test bitstreams from DIR[./CI/test_bitstreams/all_intra/]
  --teststreams-url=URL    download test bitstreams and md5sum from URL[http://openvvc.insa-rennes.fr/bitstreams/JVET_CTC/all_intra/]
```
