import sys
from pypdf import PdfReader

def main(path, out):
    r = PdfReader(path)
    lines = []
    with open(out, 'wb') as f:
        n = r.get_num_pages() if hasattr(r, 'get_num_pages') else len(r.pages)
        f.write(f'# pages: {n}\n'.encode('utf-8'))
        for i, page in enumerate(r.pages):
            f.write(f'\n===== PAGE {i+1} =====\n'.encode('utf-8'))
            try:
                t = page.extract_text() or ''
            except Exception as ex:
                t = f'<<EXTRACT ERROR: {ex}>>'
            f.write(t.encode('utf-8'))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])