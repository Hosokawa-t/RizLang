import zipfile, os

def zipdir(path, ziph):
    for root, dirs, files in os.walk(path):
        for file in files:
            ziph.write(os.path.join(root, file), os.path.relpath(os.path.join(root, file), os.path.join(path, '..')))

try:
    with zipfile.ZipFile('riz_v094_release.zip', 'w', zipfile.ZIP_DEFLATED) as zipf:
        zipf.write('riz.exe')
        zipdir('src', zipf)
        zipdir('examples', zipf)
    print("Zip created successfully")
except Exception as e:
    print(f"Error: {e}")
