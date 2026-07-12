# capstone_elem_smart-bell

## Overview

The **latest version** is currently the most stable release.

## Versions

### v1.3

* Runs independently without requiring additional files or directories.

### v1.5

* Requires external resources to function properly:

  * HTML files from `/ui`
  * Data files from `/data`
* If the `/data` directory is not available, the ESP32 will automatically generate a new one in the `/root` directory.

### v1.5fixed

* A **semi-working variant** with improvements over v1.5
* Includes an **updated UI**
* Features **better data handling and system management**
* Still requires:

  * `/ui` for interface files
  * `/data` for storage (auto-generated if missing)

## Prerequisites

Ensure the following directories and components are available:

* `/ui/dashboard`
* `/ui/login`
