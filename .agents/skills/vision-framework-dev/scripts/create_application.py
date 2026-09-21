import argparse
import json
import sys
from common import create

def main():
    p = argparse.ArgumentParser(description="Create a separate Qt simulated workstation; never overwrite existing files")
    p.add_argument("--framework-root", required=True)
    p.add_argument("--destination", required=True)
    p.add_argument("--name", required=True)
    a = p.parse_args()
    try:
        print(json.dumps(create("application", a.framework_root, a.destination, a.name), ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2
if __name__ == "__main__":
    raise SystemExit(main())
