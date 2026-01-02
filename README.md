# adrive - A Simple Artifactory CLI

A C command-line interface for interacting with Artifactory. This tool simplifies uploading, listing,
and downloading artifacts from the `scratch_US` repository.

## Prerequisites

*   cJSON, libcurl and libcrypto

## Installation

To install the necessary libraries

```bash
sudo apt update
sudo apt install libcurl4-openssl-dev libcjson-dev libssl-dev
```

## Compilation

```bash
clang -o adrive adrive.c -Wall -Wextra -O3 -lcurl -lcjson -lcrypto
```

To install the tool and use it globally, run the `install.sh` script:

```bash
./install.sh
```

After running the script, restart your terminal or source your `.bashrc`. You can then use the `adrive` command directly.

## Configuration

Before using the tool, you must set the following environment variables with your Artifactory credentials:

*   `ARTIFACTORY_USER`: Your Artifactory username.
*   `ARTIFACTORY_API_KEY`: Your Artifactory API key.

You can set these in your shell.

```bash
export ARTIFACTORY_USER="your_username"
export ARTIFACTORY_API_KEY="your_api_key"
```

## Usage

```bash
adrive <command> [options]
```

### Commands

#### 1. Upload

Uploads a local file to the Artifactory repository. The tool automatically appends a short SHA1 hash to the filename to ensure uniqueness.

**Syntax:**
```bash
adrive upload [options] <file>
```

**Options:**
*   `file`: The local file to upload (required).
*   `--dest-path`: The destination path inside the repository. Defaults to your `ARTIFACTORY_USER` name if omitted.
*   `--filename`: Override the filename stored in Artifactory.

**Examples:**

Upload to your user folder:
```bash
adrive upload my_app.apk
```

Upload to a specific folder:
```bash
adrive upload --dest-path "team/builds" my_app.apk
```

#### 2. List

Lists files in a specific path within the repository.

**Syntax:**
```bash
adrive list [options]
```

**Options:**
*   `--path`: The path in the repository to list. Defaults to your `ARTIFACTORY_USER` name if omitted.

**Examples:**

List files in your user folder:
```bash
adrive list
```

List files in a specific folder:
```bash
adrive list --path "team/builds"
```

#### 3. Download

Downloads an artifact from the repository. You can identify the file by name, SHA1 ID, or simply request the latest file.

**Syntax:**
```bash
adrive download [options]
```

**Options:**
*   `--path`: The path in the repository where the file is located. Defaults to your `ARTIFACTORY_USER` name if omitted (unless using `--id`).
*   `--name`: The exact filename to download.
*   `--id`: The SHA1 hash of the file. This searches the entire repository, so `--path` is ignored.
*   `--last`: Download the most recently modified file in the specified path.
*   `--extract`: Extracts the downloaded file using some program from user choice.
*   `--out`: The local output filename. Defaults to the artifact name.

**Examples:**

Download a specific file from your user folder:
```bash
adrive download --name "my_app__a1b2c3d4.apk"
```

Download the latest file from your user folder:
```bash
adrive download --last
```

Download a file by its SHA1 hash (searches everywhere):
```bash
adrive download --id "a1b2c3d4e5f6..."
```

Download to a specific local file:
```bash
adrive download --last --out "latest_build.apk"
```
