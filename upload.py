"""Retired legacy uploader. Packaging now belongs to build.py."""
if __name__ == "__main__":
    raise SystemExit(
        "upload.py is retired. Run build.py to create validated installation and "
        "symbol ZIPs under build/<target>/dist/. Use the Release workflow for "
        "optional publication; this script will not send credentials or files."
    )
