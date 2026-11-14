from pathlib import Path
import hashlib,json,sys
root=Path(__file__).resolve().parent.parent
lock=json.loads((root/'third_party/lock.json').read_text())
errors=[]
for name,dependency in lock.items():
 for filename,expected in dependency['files'].items():
  path=root/filename
  if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=expected: errors.append(filename)
if errors: print('Dependency checksum mismatch: '+', '.join(errors)); sys.exit(1)
print('Pinned dependency checksums verified')
