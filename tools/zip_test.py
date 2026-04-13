import zipfile, os

def zipdir(path, ziph):
    for root, dirs, files in os.walk(path):
        for file in files:
            fpath = os.path.join(root, file)
            print(f"Adding {fpath}")
            ziph.write(fpath, os.path.relpath(fpath, os.path.join(path, '..')))

try:
    with zipfile.ZipFile('riz_v095_test.zip', 'w', zipfile.ZIP_DEFLATED) as zipf:
        zipf.write('riz.exe')
        zipdir('src', zipf)
        zipdir('examples', zipf)
    print("Zip created successfully")
except Exception as e:
    print(f"Error: {e}")
