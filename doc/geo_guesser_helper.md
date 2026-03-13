# Geo guesser helper contract

Darktable's geotagging module can call an external helper to guess image
coordinates from photo content.

## Configure the command

Set `plugins/lighttable/geotagging/geo_guesser_command` to the helper command.
Darktable appends these arguments for each selected image:

- `--image /absolute/path/to/image`
- `--imgid 123`

The configured command can include additional fixed arguments. Example:

`python3 /path/to/tools/geo_guesser_mock.py --place "Paris, France" --latitude 48.8566 --longitude 2.3522`

## Required stdout format

The helper must print a single JSON object to stdout and exit with status `0`.
At minimum it must contain:

- `latitude`
- `longitude`

Optional fields:

- `elevation`
- `place`
- `confidence`
- `reasoning`

Example response:

`{"latitude": 48.8566, "longitude": 2.3522, "place": "Paris, France", "confidence": 0.78, "reasoning": "Street furniture and rooflines match central Paris."}`

If the helper fails, it should exit non-zero and write an error message to
stderr.

## Recommended real-world helpers

The helper is intentionally external so it can use any workflow you trust:

- a local ONNX or other vision model,
- a scripted pipeline that renders a preview first,
- or a hosted multimodal model that can inspect the image and perform web
  research before returning coordinates.

For raw inputs, a helper can render a temporary preview itself before sending
the image to a model or research service.
