import argparse
import errno
import glob
import os
import shutil
import sys

def copytree(src, dst):
  if os.path.isdir(src):
      if not os.path.exists(dst):
          mkdir_p(dst)
      for name in os.listdir(src):
          copytree(os.path.join(src, name),
                   os.path.join(dst, name))
  else:
      print("COPY: " + dst)
      shutil.copyfile(src, dst)
      
def mkdir_p(path):
  print("CREATE: " + path)
  try:
    os.makedirs(path)
    
  except OSError as exception:
    if exception.errno != errno.EEXIST:
      raise
#todo: this script needs to be broken up into multiple methods 
#      and should be ported to be more like a class

#####################################################################
# Parse arguments
#####################################################################
parser = argparse.ArgumentParser(description='Bundle up some RetroFE common files.')
parser.add_argument('--os', choices=['windows','linux','mac'], required=True, help='Operating System (windows or linux or mac)')
parser.add_argument('--build', choices=['full', 'core', 'engine', 'layout', 'none'], default='full', help='Define what contents to package (full, core, engine, layout, none')
parser.add_argument('--clean', action='store_true', help='Clean the output directory')
parser.add_argument('--build-directory', default='RetroFE/Build', help='CMake build directory, relative to the repository or absolute')
parser.add_argument('--configuration', default='Release', help='Multi-configuration build variant')

args = parser.parse_args()

#####################################################################
# Determine base path os to build
#####################################################################
base_path = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
common_path = os.path.join(base_path, 'Package', 'Environment', 'Common')
os_path = None

if args.os == 'windows':
  os_path = os.path.join(base_path, 'Package', 'Environment', 'Windows')
  
elif args.os == 'linux':
  os_path = os.path.join(base_path, 'Package', 'Environment', 'Linux')

elif args.os == 'mac':
  os_path = os.path.join(base_path, 'Package', 'Environment', 'MacOS')

#####################################################################
# Copy layers, artwork, config files, etc for the given os
#####################################################################
output_path = os.path.join(base_path, 'Artifacts', args.os, 'RetroFE')

if os.path.exists(output_path) and args.clean:
  shutil.rmtree(output_path)


if args.build != 'none' and not os.path.exists(output_path):
  os.makedirs(output_path)

if args.build == 'full':
  collection_path = os.path.join(output_path, 'collections')
  copytree(common_path, output_path)
  for name in os.listdir(os_path):
    if args.os == 'windows' and name == 'retrofe':
      continue
    copytree(os.path.join(os_path, name), os.path.join(output_path, name))
  
  mkdir_p(os.path.join(output_path, 'meta', 'mamelist'))
  
  dirs = [d for d in os.listdir(collection_path) if os.path.isdir(os.path.join(collection_path, d))]
  for collection in dirs:
    if not collection.startswith('_'):
     mkdir_p(os.path.join(output_path, 'collections', collection, 'roms'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'artwork_front'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'artwork_back'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'medium_back'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'medium_front'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'bezel'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'logo'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'screenshot'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'screentitle'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'medium_artwork', 'video'))
     mkdir_p(os.path.join(output_path, 'collections', collection, 'system_artwork'))
 
elif args.build == 'layout':
  layout_dest_path = os.path.join(output_path, 'layouts')
  layout_common_path = os.path.join(common_path, 'layouts')
  layout_os_path = os.path.join(os_path, 'layouts')
  
  if not os.path.exists(layout_dest_path):
    os.makedirs(layout_dest_path)

  if os.path.exists(layout_common_path):
    copytree(layout_common_path, layout_dest_path)
    
  if os.path.exists(layout_os_path):
    copytree(layout_os_path, layout_dest_path)

#####################################################################
# Copy retrofe executable
#####################################################################
if args.build in ('full', 'core', 'engine'):
  build_path = os.path.join(base_path, args.build_directory)
  binary_name = 'retrofe.exe' if args.os == 'windows' else 'retrofe'
  runtime_path = os.path.join(build_path, 'bin', args.configuration)
  if not os.path.isfile(os.path.join(runtime_path, binary_name)):
    runtime_path = os.path.join(build_path, 'bin')
  src_exe = os.path.join(runtime_path, binary_name)
  if not os.path.isfile(src_exe):
    raise SystemExit('Build RetroFE first; missing executable: ' + src_exe)
  core_path = os.path.join(output_path, 'retrofe') if args.os == 'windows' else output_path
  mkdir_p(core_path)
  if args.os == 'windows':
    manifest = os.path.join(runtime_path, 'runtime-files.txt')
    if not os.path.isfile(manifest):
      raise SystemExit('Missing staged runtime manifest; run RetroFE/Source/Build.ps1 first')
    with open(manifest, encoding='utf-8') as entries:
      for entry in entries:
        relative = entry.strip()
        if not relative:
          continue
        # A manifest may name nested license files but never leave the runtime.
        if os.path.isabs(relative) or '..' in relative.replace('\\', '/').split('/'):
          raise SystemExit('Invalid runtime manifest path: ' + relative)
        destination = os.path.join(core_path, relative)
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        shutil.copy2(os.path.join(runtime_path, relative), destination)
    shutil.copy2(manifest, core_path)
    for legacy in ('SDL2.dll', 'SDL2_image.dll', 'SDL2_mixer.dll', 'SDL2_ttf.dll'):
      legacy_path = os.path.join(core_path, legacy)
      if os.path.isfile(legacy_path):
        os.remove(legacy_path)
  shutil.copy2(src_exe, core_path)
