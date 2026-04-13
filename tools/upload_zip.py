import zipfile
import os
import subprocess

def create_zip():
    try:
        with zipfile.ZipFile('riz_v0.9.4_release.zip', 'w', zipfile.ZIP_DEFLATED) as z:
            print("Adding riz.exe")
            z.write("riz.exe")
            
            for root, _, files in os.walk("src"):
                for f in files:
                    if f.endswith('.c') or f.endswith('.h'):
                        fp = os.path.join(root, f)
                        z.write(fp, os.path.join("src", f))
                        
            for root, _, files in os.walk("examples"):
                for f in files:
                    # Skip .exe and raw .obj files, but include .riz, .cpp, .c, .md, .dll
                    if f.endswith('.exe') or f.endswith('.obj') or f.endswith('.lib') or f.endswith('.exp'):
                        continue
                    fp = os.path.join(root, f)
                    z.write(fp, os.path.relpath(fp, '.'))
            
            # Include root scripts for environment setup
            for script in ['setup.bat', 'setup_libtorch.ps1', 'CMakeLists.txt', 'build_torch.bat']:
                if os.path.exists(script):
                    z.write(script)
                        
        print("Zip created. Uploading...")
        subprocess.run(["gh", "release", "upload", "v0.9.4", "riz_v0.9.4_release.zip", "--clobber"], check=True)
        print("Upload complete!")
    except Exception as e:
        print("Error:", e)

if __name__ == "__main__":
    create_zip()
