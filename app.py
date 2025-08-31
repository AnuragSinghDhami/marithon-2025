
#!/usr/bin/env python3
import os
import json
import csv
import io
from pathlib import Path
from typing import Dict, Any, List
from datetime import datetime

# --- Libraries needed ---
# pip install google-generativeai flask python-dotenv
from dotenv import load_dotenv
from flask import Flask, render_template, request, Response, session, redirect, url_for
from werkzeug.utils import secure_filename

import google.generativeai as genai

# ----------------------------- CONFIG ---------------------------------
# Load environment variables from .env file
load_dotenv()

# --- API Configuration ---
API_KEY = os.getenv("GEMINI_API_KEY")
if not API_KEY:
    raise RuntimeError("GEMINI_API_KEY environment variable not set.")

# Configure the library
genai.configure(api_key=API_KEY)

# --- Paths ---
BASE_DIR = Path(__file__).parent
UPLOADS_DIR = BASE_DIR / "uploads"
UPLOADS_DIR.mkdir(parents=True, exist_ok=True)
ALLOWED_EXTENSIONS = {'pdf', 'docx'}

# --- Flask ---
app = Flask(__name__)
app.config['SECRET_KEY'] = os.urandom(24)

# --- Enhanced Prompt Configuration ---
ENHANCED_SOF_EXTRACTION_PROMPT = """
You are an expert AI assistant specializing in maritime logistics and shipping documentation. Your task is to meticulously extract comprehensive information from the provided 'Statement of Facts' (SoF) document.

### REQUIRED EXTRACTIONS:

1. **SHIP DETAILS**:
    - Ship/Vessel Name
    - Owner Details (company name, contact info if available)
    - Captain/Master Details (name, contact if available)
    - Ship Arrival Time (at port/berth)
    - Ship Departure Time (from port/berth)
    - IMO Number (if available)
    - Flag State (if available)

2. **PORT OPERATIONS EVENTS**:
    Extract all port operations events following these rules:

### Event Identification Rules:

You must identify and extract two types of events from the document text. Both are equally important.

1.  **Duration Events**: These are events with an explicit start and end time provided in the text.
    * *Example*: "Waiting for Lighters from 14:00 to 18:00"

2.  **Milestone Events**: These are critical, instantaneous events that are marked by only a single timestamp. You **MUST** capture these.
    * *Examples*: "Pilot on Board at 09:20", "Vessel Sailed at 19:45", "Commenced Loading at 12:00".
    * *How to process*: When you extract a milestone event, capture its single timestamp as the `start_time`. The `end_time` will be inferred later according to **Rule #2** under Edge Case Handling.

### Edge Case Handling Rules:

1. *Data Structure*: Store events in a structured list format inside a JSON object. Each event must include "event", "start_time", and "end_time".

2. *Missing Start/End Times*:
    - First, sort all events by start_time (if missing, then by end_time).
    - If start_time is missing, infer it from the previous event's end_time after sorting.
    - If end_time is missing, infer it from the next event's start_time.

3. *Rest/Break Periods*:
    - If a break period exists (e.g., Rest 12:00 - 14:00), include it as a separate event "event": "Rest period".
    - Ensure the next event does not start before the rest ends.

4. *Arrival/Departure Times*:
    - Include vessel arrival and departure times in ship_details, not in events.

5. *Unreadable/Misprinted Data*:
    - If times or events cannot be determined due to misprint/blurred text, skip them and add them separately under "unresolved_events".

6. *Overlaps / Same Time Events*:
    - If multiple events overlap or share the same timestamp, list all of them separately.

7. *Rules*:
    - Do not output any event where start_time == end_time.
    - Never invent or guess times. If you cannot resolve, move the event to "unresolved_events".
    - Skip events explicitly marked "as per charter party (CP)".
    - Never output false information. Skipping is allowed, fabrication is not.

### OUTPUT FORMAT:

Always return valid JSON with this exact structure:

{
  "ship_details": {
    "vessel_name": "MV OCEAN EXPLORER",
    "owner": "Maritime Shipping Ltd.",
    "captain": "Captain John Smith",
    "arrival_time": "2024-08-20 08:30",
    "departure_time": "2024-08-22 16:45",
    "imo_number": "IMO1234567",
    "flag_state": "Panama"
  },
  "events": [
    {
      "event": "Cargo Loading Operation",
      "start_time": "2024-08-20 14:30",
      "end_time": "2024-08-21 02:00"
    },
    {
      "event": "Rest period",
      "start_time": "2024-08-21 02:00",
      "end_time": "2024-08-21 04:00"
    }
  ],
  "unresolved_events": [
    {
      "event": "Unreadable entry at page 5 - please check manually"
    }
  ]
}

Now, analyze the following SoF document and provide the structured JSON output according to these rules.
"""

# --------------------------- UTILITIES ---------------------------------

def allowed_file(filename: str) -> bool:
    """Checks if the uploaded file has an allowed extension."""
    return '.' in filename and \
           filename.rsplit('.', 1)[1].lower() in ALLOWED_EXTENSIONS

def calculate_analysis_metrics(extracted_data: Dict[str, Any]) -> Dict[str, Any]:
    """Calculate analysis metrics for the extraction results."""
    events = extracted_data.get("events", [])
    unresolved_events = extracted_data.get("unresolved_events", [])
    
    total_events_found = len(events) + len(unresolved_events)
    successfully_parsed = len(events)
    skipped_events = len(unresolved_events)
    
    success_rate = (successfully_parsed / total_events_found * 100) if total_events_found > 0 else 0
    
    return {
        "total_events_found": total_events_found,
        "successfully_parsed": successfully_parsed,
        "skipped_events": skipped_events,
        "success_rate": round(success_rate, 2),
        "parsing_timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    }

def extract_sof_data_from_file(filepath: Path, model_name: str) -> Dict[str, Any]:
    """
    Uploads a document file (PDF, DOCX, etc.) to the Gemini API and extracts
    structured data from it using the specified multimodal model.
    """
    print(f"Uploading and processing file: {filepath.name} with model: {model_name}")
    uploaded_file = None
    try:
        # 1. Upload the file to the Gemini API
        uploaded_file = genai.upload_file(path=filepath)

        # 2. Initialize the generative model with a JSON response type
        model = genai.GenerativeModel(
            model_name,
            generation_config=genai.GenerationConfig(
                response_mime_type="application/json"
            )
        )
       
        # 3. Send the prompt and the uploaded file to the model
        response = model.generate_content([ENHANCED_SOF_EXTRACTION_PROMPT, uploaded_file])
       
        extracted_data = json.loads(response.text)
        
        # Add analysis metrics
        extracted_data["analysis"] = calculate_analysis_metrics(extracted_data)
        
        return extracted_data

    except Exception as e:
        print(f"Error during AI data extraction: {e}")
        return {"error": f"Failed to extract structured data from the document. AI model error: {e}"}
   
    finally:
        # 4. Clean up by deleting the file from the API storage
        if uploaded_file:
            print(f"Deleting uploaded file: {uploaded_file.name}")
            genai.delete_file(uploaded_file.name)

# ------------------------------ FLASK ROUTES ---------------------------------

@app.get("/")
def index() -> str:
    """Renders the main page with the file upload form."""
    return render_template("index.html")

@app.post("/")
def upload_and_process_sof() -> str:
    """Processes the uploaded SoF document and displays the extracted data."""
    if 'sof_document' not in request.files:
        return redirect(request.url)

    file = request.files['sof_document']
    pdf_type = request.form.get('pdf_type')
    render_ctx = {"filename": file.filename}

    if file.filename == '':
        render_ctx["error"] = "No file selected. Please choose a PDF or Word document."
        return render_template("index.html", **render_ctx)

    if not pdf_type:
        render_ctx["error"] = "Please select a processing type (Text-Based or Photo-Based)."
        return render_template("index.html", **render_ctx)

    if file and allowed_file(file.filename):
        filename = secure_filename(file.filename)
        filepath = UPLOADS_DIR / filename
       
        # 1. Save the file to the 'uploads' directory
        file.save(filepath)

        # 2. Choose the model based on the button clicked in the form
        model_to_use = "gemini-1.5-flash" if pdf_type == 'text' else "gemini-2.5-pro"

        # 3. Use the AI model to process the file and extract structured data
        extracted_data = extract_sof_data_from_file(filepath, model_to_use)

        # 4. Store data in session for downloading and render the results page
        if "error" in extracted_data:
            render_ctx["error"] = extracted_data["error"]
        else:
            session['sof_data'] = extracted_data
            render_ctx.update({
                'ship_details': extracted_data.get("ship_details", {}),
                'events': extracted_data.get("events", []),
                'unresolved_events': extracted_data.get("unresolved_events", []),
                'analysis': extracted_data.get("analysis", {})
            })

        return render_template("result.html", **render_ctx)
    else:
        render_ctx["error"] = "Invalid file type. Please upload a PDF (.pdf) or Word (.docx) file."
        return render_template("index.html", **render_ctx)

@app.get("/download/<filetype>")
def download_file(filetype: str):
    """Handles downloading the extracted data as JSON or CSV."""
    sof_data = session.get('sof_data', {})
    if not sof_data:
        return redirect(url_for('index'))

    if filetype == 'json':
        # Create a JSON response
        return Response(
            json.dumps(sof_data, indent=2),
            mimetype="application/json",
            headers={"Content-Disposition": "attachment;filename=sof_complete_data.json"}
        )
    elif filetype == 'csv':
        # Create a CSV response for events
        events = sof_data.get('events', [])
        output = io.StringIO()
        if events:
            writer = csv.DictWriter(output, fieldnames=events[0].keys())
            writer.writeheader()
            writer.writerows(events)
        return Response(
            output.getvalue(),
            mimetype="text/csv",
            headers={"Content-Disposition": "attachment;filename=sof_events.csv"}
        )

    return redirect(url_for('index'))

# ------------------------------ MAIN -----------------------------------
# if __name__ == "__main__":
#     app.run(host="0.0.0.0", port=8081, debug=True)
