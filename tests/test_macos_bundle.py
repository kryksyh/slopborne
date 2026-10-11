"""Architecture-separated packaging with synthetic Mach-O metadata; originals retained."""
from paths import ROOT
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('macbundle', ROOT / 'packaging/macos/bundle_dylibs.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='bundle-test-', dir=ROOT / 'out'))
        self.app = self.root / 'Test.app'
        (self.app / 'Contents/MacOS').mkdir(parents=True)
        self.tools = patch.object(bundle, 'run', side_effect=self.run_tool)
        self.tools.start()
        self.addCleanup(self.tools.stop)

    def binary(self, path, arch, deps=(), rpaths=(), identity=''):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(dict(arch=arch, deps=list(deps), rpaths=list(rpaths), identity=identity)))
        return path

    def run_tool(self, *args):
        tool, option, *rest = args
        path = Path(rest[-1])
        data = json.loads(path.read_text())
        if tool == 'lipo': return ' '.join(data['arch']) + '\n'
        if tool == 'otool':
            if option == '-L':
                deps = ([data['identity']] if data['identity'] else []) + data['deps']
                return str(path)+':\n'+''.join('\t'+d+' (compatibility version 1.0.0)\n' for d in deps)
            if option == '-D': return str(path)+':\n'+(data['identity']+'\n' if data['identity'] else '')
            if option == '-l': return ''.join('cmd LC_RPATH\ncmdsize 48\npath '+r+' (offset 12)\n' for r in data['rpaths'])
        self.assertEqual(tool, 'install_name_tool')
        if option == '-id': data['identity'] = rest[0]
        elif option == '-change': data['deps'] = [rest[1] if d == rest[0] else d for d in data['deps']]
        elif option == '-delete_rpath': data['rpaths'].remove(rest[0])
        elif option == '-add_rpath': data['rpaths'].append(rest[0])
        else: self.fail(str(args))
        path.write_text(json.dumps(data))
        return ''

    def test_same_library_name_for_two_architectures_stays_separate(self):
        originals = []
        for arch, name in [('x86_64','bb-probe'), ('arm64','bb-render')]:
            lib = self.binary(self.root / arch / 'libSDL3.dylib', [arch], ['/usr/lib/libSystem.B.dylib'])
            originals.append((lib,lib.read_bytes()))
            self.binary(self.app / 'Contents/MacOS' / name, [arch], ['@rpath/libSDL3.dylib'], [str(lib.parent)])
        result = bundle.bundle(self.app, ['bb-probe','bb-render'])
        for arch, name in [('x86_64','bb-probe'), ('arm64','bb-render')]:
            data=json.loads((result/arch/'libSDL3.dylib').read_text())
            self.assertEqual(data['arch'],[arch])
            self.assertEqual(data['rpaths'],['@loader_path'])
            root=json.loads((self.app/'Contents/MacOS'/name).read_text())
            self.assertEqual(root['rpaths'],[f'@executable_path/../Frameworks/game/{arch}'])
        for path, content in originals: self.assertEqual(path.read_bytes(), content)

    def test_loader_relative_transitive_library_is_copied(self):
        b=self.binary(self.root/'libs/libB.dylib',['arm64'])
        a=self.binary(self.root/'libs/libA.dylib',['arm64'],['@loader_path/libB.dylib'])
        self.binary(self.app/'Contents/MacOS/bb-render',['arm64'],[str(a)])
        result=bundle.bundle(self.app,['bb-render'])
        self.assertTrue((result/'arm64'/b.name).is_file())
        copied=json.loads((result/'arm64'/a.name).read_text())
        self.assertEqual(copied['deps'],['@rpath/libB.dylib'])
        self.assertEqual(json.loads(a.read_text())['deps'],['@loader_path/libB.dylib'])

    def test_wrong_architecture_is_rejected(self):
        lib=self.binary(self.root/'libs/libA.dylib',['x86_64'])
        self.binary(self.app/'Contents/MacOS/bb-render',['arm64'],[str(lib)])
        with self.assertRaisesRegex(ValueError,'contains'):
            bundle.bundle(self.app,['bb-render'])

    def test_conflicting_libraries_in_same_architecture_are_rejected(self):
        a=self.binary(self.root/'one/libA.dylib',['arm64'])
        b=self.binary(self.root/'two/libA.dylib',['arm64'],['/usr/lib/libSystem.B.dylib'])
        self.binary(self.app/'Contents/MacOS/first',['arm64'],[str(a)])
        self.binary(self.app/'Contents/MacOS/second',['arm64'],[str(b)])
        with self.assertRaisesRegex(ValueError,'Different libraries share'):
            bundle.bundle(self.app,['first','second'])


if __name__ == '__main__':
    unittest.main()
