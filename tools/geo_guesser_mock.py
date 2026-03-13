#!/usr/bin/env python3

import argparse
import json
import os
import sys


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Mock geo guesser helper for darktable")
    parser.add_argument("--image", required=True, help="Input image path appended by darktable")
    parser.add_argument("--imgid", help="Image id appended by darktable")
    parser.add_argument("--latitude", type=float, default=48.8566)
    parser.add_argument("--longitude", type=float, default=2.3522)
    parser.add_argument("--elevation", type=float)
    parser.add_argument("--place", default="Paris, France")
    parser.add_argument("--confidence", type=float, default=0.75)
    parser.add_argument(
        "--reasoning",
        default="Mock helper configured for testing darktable geo guesser plumbing.",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()

    if not os.path.exists(args.image):
        print(f"image does not exist: {args.image}", file=sys.stderr)
        return 2

    payload = {
        "latitude": args.latitude,
        "longitude": args.longitude,
        "place": args.place,
        "confidence": args.confidence,
        "reasoning": args.reasoning,
        "source_image": os.path.basename(args.image),
    }
    if args.elevation is not None:
        payload["elevation"] = args.elevation

    json.dump(payload, sys.stdout)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
