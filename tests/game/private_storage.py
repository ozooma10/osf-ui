"""Map production Settings storage for one handshaken, disposable MO2 game.

Uses MO2's installed USVFS controller API, never resets or creates its VFS.
API reference: https://github.com/ModOrganizer2/usvfs/blob/master/include/usvfs/usvfs.h
The SFSE acceptance consumer waits in Preload until this controller acknowledges
the mappings, so production Settings has not initialized or written values yet.
"""
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import uuid


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2), encoding='utf-8')
    os.replace(temporary, path)


def read_json(path):
    try:
        return json.loads(path.read_text(encoding='utf-8-sig'))
    except FileNotFoundError:
        return None


def main(config_path):
    config = read_json(config_path)
    probe = Path(config['directory']).resolve(strict=True)
    token = config['session']
    uuid.UUID(token)
    storage = config['storage']
    receipt = {'session': token, 'accepted': False}
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]

    def identity(pid):
        handle = kernel.OpenProcess(0x1400, False, pid)
        if not handle:
            raise c.WinError(c.get_last_error())
        try:
            name = c.create_unicode_buffer(32768)
            size = w.DWORD(len(name))
            created, exited, kt, ut = (w.FILETIME() for _ in range(4))
            if not kernel.GetProcessTimes(handle, c.byref(created), c.byref(exited), c.byref(kt), c.byref(ut)):
                raise c.WinError(c.get_last_error())
            if not kernel.QueryFullProcessImageNameW(handle, 0, name, c.byref(size)):
                raise c.WinError(c.get_last_error())
            return Path(name.value).resolve(), (created.dwHighDateTime << 32) | created.dwLowDateTime
        finally:
            kernel.CloseHandle(handle)

    try:
        deadline = min(time.time() + 180, storage['expiresUnixMs'] / 1000)
        ready = None
        while time.time() < deadline:
            ready = read_json(probe / 'storage-ready.json')
            if ready and ready.get('session') == token:
                break
            time.sleep(.05)
        else:
            raise RuntimeError('Owned preload handshake did not arrive before expiry')
        game = Path(storage['game']).resolve(strict=True)
        exe, created = identity(ready['pid'])
        if exe != game / 'Starfield.exe' or str(created) != ready['processStartFileTime']:
            raise RuntimeError('Preload process identity does not match the configured game')

        # Resolve the real Documents known folder independently of supplied paths.
        shell = c.WinDLL('shell32')
        guid = (c.c_ubyte * 16).from_buffer_copy(uuid.UUID('FDD39AD0-238F-46AF-ADB4-6C85480369C7').bytes_le)
        pointer = w.LPWSTR()
        shell.SHGetKnownFolderPath.argtypes = [c.c_void_p, w.DWORD, w.HANDLE, c.POINTER(w.LPWSTR)]
        if shell.SHGetKnownFolderPath(c.byref(guid), 0, None, c.byref(pointer)) != 0:
            raise RuntimeError('Could not resolve Documents')
        documents = Path(pointer.value)
        ole = c.WinDLL('ole32')
        ole.CoTaskMemFree.argtypes = [c.c_void_p]
        ole.CoTaskMemFree(c.cast(pointer, c.c_void_p))
        allowed = {documents / 'My Games/Starfield/OSF/Settings', documents / 'My Games/Starfield/Starcade'}
        for item in storage['mappings']:
            source, destination = Path(item['source']).resolve(strict=True), Path(item['destination']).resolve()
            if not source.is_relative_to(probe.parent / 'private-values') or destination not in allowed:
                raise RuntimeError('Mapping escapes the private source or approved data directories')
            if read_json(source / '.osf-ui-storage.json') != {'session': token}:
                raise RuntimeError('Private source ownership marker is missing')
            if any(path.is_symlink() or path.is_junction() for path in [source, *source.parents]):
                raise RuntimeError('Private source contains a redirected ancestor')

        mo = Path(storage['mo2']).resolve(strict=True)
        cookie = os.add_dll_directory(str(mo))
        dll = mo / 'usvfs_x64.dll'
        vfs = c.WinDLL(str(dll))
        vfs.usvfsCreateParameters.restype = c.c_void_p
        parameters = vfs.usvfsCreateParameters()
        vfs.usvfsSetInstanceName.argtypes = [c.c_void_p, c.c_char_p]
        vfs.usvfsSetInstanceName(parameters, storage['vfsInstance'].encode('ascii'))
        vfs.usvfsConnectVFS.argtypes = [c.c_void_p]
        vfs.usvfsConnectVFS.restype = w.BOOL
        if not vfs.usvfsConnectVFS(parameters):
            raise RuntimeError('Could not connect to the existing MO2 VFS')
        vfs.usvfsGetVFSProcessList.argtypes = [c.POINTER(c.c_size_t), c.POINTER(w.DWORD)]
        pids, count = (w.DWORD * 1024)(), c.c_size_t(1024)
        if not vfs.usvfsGetVFSProcessList(c.byref(count), pids) or count.value > 1024:
            raise RuntimeError('Could not establish the VFS process inventory')
        receipt['vfsPids'] = list(pids[:count.value])
        if ready['pid'] not in pids[:count.value]:
            raise RuntimeError('Owned game is not in the selected VFS')
        allowed_processes = {game / 'Starfield.exe', game / 'sfse_loader.exe', mo / 'ModOrganizer.exe',
                             mo / 'usvfs_proxy_x64.exe', Path(sys.executable).resolve()}
        inventory = []
        for pid in pids[:count.value]:
            try:
                path, creation = identity(pid)
            except OSError:
                continue  # An exited loader is harmless.
            inventory.append({'pid': pid, 'path': str(path), 'creation': str(creation)})
            if path not in allowed_processes or (path == game / 'Starfield.exe' and pid != ready['pid']):
                raise RuntimeError(f'Unexpected process in test VFS: {pid} {path}')

        vfs.usvfsVirtualLinkDirectoryStatic.argtypes = [w.LPCWSTR, w.LPCWSTR, c.c_uint]
        vfs.usvfsVirtualLinkDirectoryStatic.restype = w.BOOL
        for item in storage['mappings']:
            # CREATETARGET redirects new/temp/renamed files; RECURSIVE maps the
            # complete private copy, including existing files, before any reads.
            if not vfs.usvfsVirtualLinkDirectoryStatic(item['source'], item['destination'], 4 | 8):
                raise RuntimeError('USVFS rejected the storage mapping')
        receipt.update(accepted=True, pid=ready['pid'], processStartFileTime=str(created),
                       mappings=storage['mappings'], processes=inventory,
                       usvfsSha256=hashlib.sha256(dll.read_bytes()).hexdigest())
        write_json(probe / 'storage-mapped.json', receipt)
        # Keep the controller connection until the preload verifies mapped reads.
        deadline = time.time() + 15
        while time.time() < deadline:
            verified = read_json(probe / 'storage-verified.json')
            if verified and verified.get('session') == token and verified.get('verified'):
                return
            time.sleep(.05)
        raise RuntimeError('Owned preload did not verify private storage')
    except Exception as error:
        receipt.update(accepted=False, error=str(error))
        write_json(probe / 'storage-mapped.json', receipt)
        raise


if __name__ == '__main__':
    main(Path(sys.argv[1]).resolve(strict=True))
