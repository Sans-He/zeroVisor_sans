# zeroVisor

> ***zeroVisor: A Type1.5 hypervisor skeleton —— Everything starts from zero.***

## How to use

### 1. Environment Preparation

- **Linux Kernel**  

  - Supported: Tested on **5.10.240**
  - In theory, any **5.x** or **6.x** branch will work with minor tweaks.  

- **Architecture**  

  - **x86-64** only.  

- **Verify your setup**  

  ```bash
  # Check kernel version
  uname -r
  # Example output: 5.10.0-42-generic / 5.10.240
  ```

### 2. Clone the Repository

```bash
git clone https://github.com/your-org/zeroVisor.git
cd zeroVisor
```

### 3. Build & Installation

- Our **Makefile** provides a comprehensive automated workflow, making compilation and installation very convenient.

- **Compile:**

  ```bash
  make clean && make
  ```

- **Install**:

  ```bash
  make install
  ```

## Some usages

### 1. zv_log 

- All zeroVisor log output can be read via the `/proc/zv_log` interface.

  ```bash
  cat /proc/zv_log
  ```

- If you enable the `ZEROVISOR_LOG_TO_KMSG` compile-time macro in `zv_config.h`, logs will also be sent to the kernel message buffer (kmsg), so you can view them with `dmesg`.

- The `/proc/zv_log_level` interface lets you dynamically read and set the log verbosity level, giving you fine-grained control during debugging or platform development.

  ```bash
  echo "log level = DETAIL" > /proc/zv_log_level
  ```

  Our log levels are divided into four grades: NONE, NORMAL DEBUG and DETAIL.

  
