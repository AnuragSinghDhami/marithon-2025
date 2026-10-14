# Ship Event Analytics (C++17)

A starter C++ web app for a multi-document Statement of Facts (SoF) workflow:

1. Apache Tika extracts text from a PDF or DOCX.
2. A C++ rule-based parser converts timestamped text rows into normalized events.
3. SQLite stores ships, documents, and events across uploads.
4. DSA routines deduplicate/group events, reconstruct berth stays, merge intervals, and compute fleet concurrency.
5. Gemini interprets a user's question, the backend runs a deterministic database/DSA query, and Gemini explains the result.

**Important:** Apache Tika extracts document content; it does not understand SoF business semantics by itself. The `parse_events()` and `classify_event()` functions are intentionally rule-based and must be adapted to the real row layout and terminology of your documents. The app does not ask AI to invent or extract the source events.

## Requirements

- C++17 compiler
- CMake 3.20+
- libcurl development files
- SQLite 3 development files
- Docker Compose (for Apache Tika)
- Gemini API key for natural-language questions
- Internet on the first CMake configure/build, because CMake FetchContent downloads `cpp-httplib` and `nlohmann/json`

On Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libcurl4-openssl-dev libsqlite3-dev docker-compose-plugin
```

## Run

From this folder:

```bash
cp .env.example .env
```

Edit `.env` and set `GEMINI_API_KEY`. The default model is configurable with `GEMINI_MODEL`; the sample uses `gemini-3.8-flash`.

Start Tika:

```bash
docker compose up -d tika
```

Build the C++ application:

```bash
cmake -S . -B build
cmake --build build -j
```

Run it:

```bash
./build/ship_event_analytics
```

Open `http://127.0.0.1:8081`.

The first CMake configure downloads the header-only dependencies. If you already manage those libraries locally, replace the `FetchContent` entries in `CMakeLists.txt` with your package targets.

## Example supported event rows

`sample_sof.txt` shows the expected general layout. The parser expects a date/time and event description on the same extracted text line. It supports timestamps such as:

- `2026-10-14 08:00 First line ashore`
- `14/10/2026 08:00 First line ashore`
- `14-Oct-2026 08:00 First line ashore`
- `2026-10-14 10:00 - 11:00 Lunch break` (the second time is assumed to be on the same date, or the next day if the end clock time is earlier)
- `2026-10-14 16:00 Last line cast off`

The parser supports a maximum of two timestamps per row. Rows with no recognized timestamp or more than two timestamps are skipped instead of guessed. If Tika renders a table as separate date, time, and description lines, adapt `parse_events()` to that output or add a table-aware parser.

### Scan-only PDFs

Tika may extract no text from a scanned PDF unless an OCR-capable Tika configuration/image is used. In that case, configure Tika with OCR/Tesseract and verify the text before trusting the analytics.

## What is stored

`ship_events.db` contains:

- `ships`: vessel identity (IMO is the preferred stable identity; name is the fallback)
- `documents`: uploaded file name and time
- `events`: parsed event type/name, timestamp(s), source line and document reference
- `dock_stays`: reconstructed and merged berth occupancy intervals

The SQLite file persists across server restarts. Uploads are processed in memory; the original uploaded file is not retained by this sample app.

## DSA pipeline

- Grouping and deduplication: group by ship identity; remove duplicate event rows with matching type, label and timestamps.
- Docking reconstruction: pair `berth_start` and `berth_end` milestones; preserve explicitly labelled `dock_stay` intervals.
- Merge intervals: merge overlapping/touching docking periods for the same ship.
- Sweep line: calculate maximum concurrent docked ships from reconstructed stays.
- Window query: count distinct ships whose stay overlaps `[window_start, window_end)` with `stay.start < window_end && stay.end > window_start`.

A single milestone is not enough to reconstruct a docking duration. If a berth start/end counterpart is missing, this app does not invent the missing boundary. Arrival at port is also not automatically treated as docking at a berth.

## Ask examples

- `How many ships were docked today between 10 AM and 11 AM?`
- `When was MV Ocean Explorer docked?`
- `When was lunch recorded for MV Ocean Explorer?`
- `Summarize port activity and the peak number of docked ships.`

For a time-window count with no date, the app uses the server's current local date and includes that assumption in the facts shown on screen. Configure the server timezone to match the port's local time. The code does not perform timezone conversion.

## Important limitations before production use

- SoF documents differ. Validate `parse_events()` and `classify_event()` against real files and preserve source evidence.
- Event classification uses keywords. Extend the rules for the phrasing used by your shipping agents/ports.
- The app only reconstructs berth stays from recognizable berth-start/end events or explicit docking interval labels; unmatched starts/ends remain incomplete.
- An IMO number is preferred over vessel name; ambiguous or missing identity should be resolved during upload.
- The example uses a simple local web server and unauthenticated forms. Add authentication, CSRF controls, rate limits, TLS, retention policy, and request-size controls before exposing it outside localhost.
- AI is only used to interpret questions and write explanations. Counts and interval matches come from SQLite/DSA results. The AI response should still be checked against the displayed facts.

## References

- Apache Tika Server: https://tika.apache.org/
- Gemini `generateContent`: https://ai.google.dev/api/generate-content
- Gemini model list: https://ai.google.dev/gemini-api/docs/models
- cpp-httplib: https://github.com/yhirose/cpp-httplib
- nlohmann/json: https://github.com/nlohmann/json
- SQLite C API: https://sqlite.org/cintro.html
