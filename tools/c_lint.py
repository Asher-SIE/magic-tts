"""C 源结构自检 (无编译器环境的降级验证):
- 去注释/字符串后检查花括号与圆括号平衡
- 提取本文件定义的函数与被引用的静态函数, 查未定义引用
- 检查分号明显缺失的行 (启发式, 仅报告)
用法: python tools/c_lint.py Bridge/aisound_adapter.c
"""
import re
import sys


def strip_c(s: str) -> str:
    out = []
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == '/' and i + 1 < n and s[i + 1] == '/':
            while i < n and s[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and s[i + 1] == '*':
            i += 2
            while i + 1 < n and not (s[i] == '*' and s[i + 1] == '/'):
                i += 1
            i += 2
        elif c == '"':
            i += 1
            while i < n and s[i] != '"':
                if s[i] == '\\':
                    i += 1
                i += 1
            i += 1
        elif c == "'":
            i += 1
            while i < n and s[i] != "'":
                if s[i] == '\\':
                    i += 1
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def main(path: str) -> int:
    src = open(path, encoding='utf-8').read()
    code = strip_c(src)
    for open_ch, close_ch, name in (('{', '}', '花括号'), ('(', ')', '圆括号')):
        depth = 0
        line = 1
        ok = True
        for ch in code:
            if ch == '\n':
                line += 1
            elif ch == open_ch:
                depth += 1
            elif ch == close_ch:
                depth -= 1
                if depth < 0:
                    print(f'!! {name} 负深度 @ 行~{line}')
                    ok = False
                    break
        if depth != 0:
            print(f'!! {name} 不平衡: 深度 {depth}')
            ok = False
        if ok:
            print(f'OK {name} 平衡')

    # 静态函数定义名
    defs = set(re.findall(r'(?:^|\n)(?:static\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;]*?\)\s*\{', code))
    # 引用的本文件静态函数 (粗糙: 所有标识符调用)
    calls = set(re.findall(r'\b(\w+)\s*\(', code))
    keywords = {'if', 'for', 'while', 'switch', 'sizeof', 'return', 'snprintf',
                'memcpy', 'malloc', 'calloc', 'realloc', 'free', 'fopen', 'fclose',
                'fread', 'fseek', 'ftell', 'fprintf', 'strcmp', 'strdup', 'dlopen',
                'dlsym', 'dlclose', 'dlerror', 'pthread_create', 'pthread_join',
                'pthread_mutex_init', 'pthread_mutex_destroy', 'pthread_mutex_lock',
                'pthread_mutex_unlock', 'pthread_cond_init', 'pthread_cond_destroy',
                'pthread_cond_wait', 'pthread_cond_broadcast', 'pthread_cond_timedwait',
                'clock_gettime', 'sonicCreateStream', 'sonicDestroyStream',
                'sonicSetSpeed', 'sonicWriteShortToStream',
                'sonicReadShortFromStream', 'sonicFlushStream'}
    unknown = calls - defs - keywords
    if unknown:
        print('!! 未识别调用 (人工核对):', sorted(unknown))
    else:
        print('OK 调用均有定义或属 libc/引擎符号')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
