# adrive - A Simple Artifactory CLI

A Python command-line interface for interacting with Artifactory. This tool simplifies uploading, listing,
and downloading artifacts from the `scratch_US` repository.

## Prerequisites

*   Python 3

## Installation

To install the tool and use it globally, run the `install.sh` script:

```bash
./install.sh
```

This script performs the following actions:
1.  Creates a directory at `/localrepo/$USER/.local/bin/` (if it doesn't exist).
2.  Creates a symbolic link from `adrive.py` to that directory.
3.  Updates your `~/.bashrc` to include the new directory in your `PATH`.

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

Run the script using Python (or `adrive` if installed):

```bash
python adrive.py <command> [options]
```
# OR
```bash
adrive <command> [options]
```

### Commands

#### 1. Upload

Uploads a local file to the Artifactory repository. The tool automatically appends a short SHA1 hash to the filename to ensure uniqueness.

**Syntax:**
```bash
python adrive.py upload [options] <file>
```

**Options:**
*   `file`: The local file to upload (required).
*   `--dest-path`: The destination path inside the repository. Defaults to your `ARTIFACTORY_USER` name if omitted.
*   `--filename`: Override the filename stored in Artifactory.

**Examples:**

Upload to your user folder:
```bash
python adrive.py upload my_app.apk
```

Upload to a specific folder:
```bash
python adrive.py upload --dest-path "team/builds" my_app.apk
```

#### 2. List

Lists files in a specific path within the repository.

**Syntax:**
```bash
python adrive.py list [options]
```

**Options:**
*   `--path`: The path in the repository to list. Defaults to your `ARTIFACTORY_USER` name if omitted.

**Examples:**

List files in your user folder:
```bash
python adrive.py list
```

List files in a specific folder:
```bash
python adrive.py list --path "team/builds"
```

#### 3. Download

Downloads an artifact from the repository. You can identify the file by name, SHA1 ID, or simply request the latest file.

**Syntax:**
```bash
python adrive.py download [options]
```

**Options:**
*   `--path`: The path in the repository where the file is located. Defaults to your `ARTIFACTORY_USER` name if omitted (unless using `--id`).
*   `--name`: The exact filename to download.
*   `--id`: The SHA1 hash of the file. This searches the entire repository, so `--path` is ignored.
*   `--last`: Download the most recently modified file in the specified path.
*   `--out`: The local output filename. Defaults to the artifact name.

**Examples:**

Download a specific file from your user folder:
```bash
python adrive.py download --name "my_app__a1b2c3d4.apk"
```

Download the latest file from your user folder:
```bash
python adrive.py download --last
```

Download a file by its SHA1 hash (searches everywhere):
```bash
python adrive.py download --id "a1b2c3d4e5f6..."
```

Download to a specific local file:
```bash
python adrive.py download --last --out "latest_build.apk"
```
