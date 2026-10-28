"""List the entry names in a zip exactly as stored (to check for backslash paths)."""
import sys
import zipfile

for info in zipfile.ZipFile(sys.argv[1]).infolist():
    print(repr(info.orig_filename))
