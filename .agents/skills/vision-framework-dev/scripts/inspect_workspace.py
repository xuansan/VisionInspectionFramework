import argparse
import json
import sys
from common import inspect

def main():
    parser = argparse.ArgumentParser(description="Check the framework interfaces supported by this skill (read-only)")
    parser.add_argument("--framework-root", required=True)
    args = parser.parse_args()
    try:
        result = inspect(args.framework_root)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["compatible"] else 2
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2
if __name__ == "__main__":
    raise SystemExit(main())
