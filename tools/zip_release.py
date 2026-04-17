import zipfile, os, glob

def zipdir(path, ziph):
    for root, dirs, files in os.walk(path):
        # Skip huge or irrelevant directories
        if any(x in root for x in ["build_check", "deps", "vendor", ".git", ".gemini"]): continue
        for file in files:
            # Exclude large models, archives, and other non-source binaries
            if file.endswith(".gguf") or file.endswith(".bin") or file.endswith(".zip") or file.endswith(".exe"): 
                # Keep riz.exe but skip others like bench_gpu.exe
                if file != "riz.exe": continue
            
            file_path = os.path.join(root, file)
            # Skip any file larger than 10MB just to be safe for a source/binary distribution
            if os.path.getsize(file_path) > 10 * 1024 * 1024: continue 
            
            ziph.write(file_path, os.path.relpath(file_path, os.path.join(path, '..')))

try:
    zip_name = 'riz_v0.9.8_official.zip'
    with zipfile.ZipFile(zip_name, 'w', zipfile.ZIP_DEFLATED) as zipf:
        zipf.write('riz.exe')
        # Include small plugins only
        for dll in glob.glob("*.dll"):
            if os.path.getsize(dll) < 10 * 1024 * 1024:
                zipf.write(dll)
        
        zipdir('src', zipf)
        zipdir('examples', zipf)
        zipf.write('README.md')
    print(f"Zip created successfully: {zip_name}")
except Exception as e:
    print(f"Error: {e}")
