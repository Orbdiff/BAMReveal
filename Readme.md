# BAMReveal

**BAMReveal** is a tool designed to inspect BAM entries, which are normally reviewed manually through Regedit.  
It parses BAM entries and extracts execution timestamps, executable paths, while also verifying digital signatures and detecting replaces using the USN Journal, btw it also includes Generic detections powered by YARA Rules.

The project includes:

* Digital signature verification
* Replace detection using the USN Journal
* YARA Rules integration
* BAM service timing analysis

---

## Features

### BAM Parsing

* Parses BAM entries and extracts:

  * Execution timestamps
  * Executable paths
  * Package-related entries

### Digital Signature Verification

Detects the following signature states:

* Signed
* Unsigned
* Fake Signature
* Cheat

### Path Resolution

* Automatically converts `\\Device\\` paths into standard drive-letter paths (`C:\\`, `D:\\`, etc).

> Makes the output way easier to read tbh.

### YARA Rules

* Includes Generic detections powered by YARA Rules.
* Clicking a Generic entry reveals the matched rule for easier investigation.

### Search

* Built-in search box allowing u to quickly search for anything across the table.

### Replaces Detect

Detects replaces using the USN Journal.  
Basically, if a path appears highlighted in red inside the table, it means a replace was detected.

U can click the entry to inspect the detected events and related timestamps.

Currently detects the following replace types:

* Explorer
* Type
* Copy
* Hax

---

## Filters

### `Post Logon`

Shows all entries executed after the user's logon time.

### `Show Untrusted`

Displays entries flagged as:

* Unsigned
* Cheat-related
* Fake signed

### `Show Not Found`

Displays packages and paths that couldn't be resolved or were missing from disk.

---

## Buttons

### `Deleted BAM`

Searches BAM-related paths inside the System log and compares them against current Registry entries, if an entry exists in one location but not the other, it gets flagged, helping detect possible BAM Registry entry deletion.

### `Registry BAM`

Shows BAM Registry keys with denied permissions.

### `BAM Info`

Displays:

* System boot time
* `bam.sys` creation timestamp
* When the BAM driver started after boot

> Useful for checking timing inconsistencies and possible tampering.

---

The whole tool is built using Dear ImGui, making the UI clean and much easier to understand.

No bloated UI stuff, just straight to the point!!!
