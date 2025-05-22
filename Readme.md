# BAMReveal

**BAMReveal** is a forensic tool that allows you to analyze Background Activity Moderator (BAM) entries on Windows systems.

---

## Features
- Full analysis of the BAM registry `(SYSTEM\CurrentControlSet\Services\bam\State\UserSettings)`
- Extraction of the creation time `(via bam.sys)`
- Detection of deleted paths from BAM `(by comparing registry and SYSTEM files)`
- Verification of digital signatures of executables