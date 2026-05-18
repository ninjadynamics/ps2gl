"""Minimal gasp replacement for ps2gl VCL preprocessing.

Handles: .include, .macro/.endm, .assignc, .aif/.aelse/.aendi,
         .arepeat/.aendr, .equ, backslash-param, backslash-@, backslash-&var
Passes through everything else (VCL directives, VU instructions, etc.)
"""
import sys, os, re

class GaspExpander:
    def __init__(self, include_dirs, comment_char=';'):
        self.include_dirs = include_dirs
        self.comment_char = comment_char
        self.macros = {}
        self.string_vars = {}
        self.equ_vars = {}
        self.expand_counter = 0
        self.included = set()

    def find_file(self, name):
        for d in self.include_dirs:
            p = os.path.join(d, name)
            if os.path.isfile(p):
                return p
        return None

    def strip_comment(self, line):
        in_str = False
        for i, c in enumerate(line):
            if c == '"':
                in_str = not in_str
            elif c == self.comment_char and not in_str:
                return line[:i]
        return line

    def eval_expr(self, expr):
        """Evaluate simple integer expressions like '3', 'kFoo-1', 'kFoo'."""
        expr = expr.strip()
        for name, val in self.equ_vars.items():
            expr = re.sub(r'\b' + re.escape(name) + r'\b', str(val), expr)
        try:
            return int(eval(expr))
        except:
            return 0

    def process_lines(self, lines, filename="<input>"):
        output = []
        i = 0
        while i < len(lines):
            line = lines[i]
            stripped = line.strip()

            # .include "file"
            m = re.match(r'\s*\.include\s+"([^"]+)"', stripped)
            if m:
                inc_name = m.group(1)
                inc_path = self.find_file(inc_name)
                if inc_path and inc_path not in self.included:
                    self.included.add(inc_path)
                    with open(inc_path, 'r') as f:
                        inc_lines = f.readlines()
                    output.extend(self.process_lines(inc_lines, inc_path))
                i += 1
                continue

            # .assignc name "value"
            m = re.match(r'\s*(\w+)\s+\.assignc\s+"([^"]*)"', stripped)
            if m:
                self.string_vars[m.group(1)] = m.group(2)
                i += 1
                continue

            # name .equ value — resolve and substitute, don't emit
            m = re.match(r'\s*(\w+)\s+\.equ\s+(.*)', stripped)
            if m:
                name = m.group(1)
                val_str = m.group(2).strip()
                try:
                    self.equ_vars[name] = self.eval_expr(val_str)
                except:
                    pass
                i += 1
                continue

            # .macro name params
            m = re.match(r'\s*\.macro\s+(\w+)\s*(.*)', stripped)
            if m:
                macro_name = m.group(1)
                params_str = m.group(2).strip()
                raw_params = [p.strip() for p in params_str.split(',') if p.strip()] if params_str else []
                params = []
                defaults = {}
                for rp in raw_params:
                    if '=' in rp:
                        pname, pdefault = rp.split('=', 1)
                        params.append(pname.strip())
                        defaults[pname.strip()] = pdefault.strip()
                    else:
                        params.append(rp)
                body = []
                i += 1
                depth = 1
                while i < len(lines) and depth > 0:
                    ms = lines[i].strip()
                    if re.match(r'\s*\.macro\s+', ms):
                        depth += 1
                    if re.match(r'\s*\.endm', ms):
                        depth -= 1
                        if depth == 0:
                            i += 1
                            break
                    body.append(lines[i])
                    i += 1
                self.macros[macro_name] = (params, body, defaults)
                continue

            # .arepeat count / .aendr
            m = re.match(r'\s*\.arepeat\s+(.*)', stripped)
            if m:
                count_expr = m.group(1).strip()
                count = self.eval_expr(count_expr)
                body = []
                i += 1
                depth = 1
                while i < len(lines) and depth > 0:
                    s = lines[i].strip()
                    if re.match(r'\s*\.arepeat\s+', s):
                        depth += 1
                        body.append(lines[i])
                    elif re.match(r'\s*\.aendr', s):
                        depth -= 1
                        if depth == 0:
                            i += 1
                            break
                        body.append(lines[i])
                    else:
                        body.append(lines[i])
                    i += 1
                for _ in range(max(0, count)):
                    output.extend(self.process_lines(body, filename))
                continue

            # .aif / .aelse / .aendi (top-level conditionals)
            m = re.match(r'\s*\.aif\s+(.*)', stripped)
            if m:
                cond = m.group(1).strip()
                true_block = []
                false_block = []
                current = true_block
                i += 1
                depth = 1
                while i < len(lines) and depth > 0:
                    s = lines[i].strip()
                    if re.match(r'\s*\.aif\s+', s):
                        depth += 1
                        current.append(lines[i])
                    elif re.match(r'\s*\.aelse', s) and depth == 1:
                        current = false_block
                    elif re.match(r'\s*\.aendi', s):
                        depth -= 1
                        if depth == 0:
                            i += 1
                            break
                        current.append(lines[i])
                    else:
                        current.append(lines[i])
                    i += 1
                if self.eval_condition(cond):
                    output.extend(self.process_lines(true_block, filename))
                else:
                    output.extend(self.process_lines(false_block, filename))
                continue

            # Try macro invocation: first word matches a known macro
            m = re.match(r'\s*(\w+)\s*(.*)', stripped)
            if m and m.group(1) in self.macros:
                macro_name = m.group(1)
                args_str = m.group(2).strip()
                args = [a.strip() for a in args_str.split(',') if a.strip()] if args_str else []
                expanded = self.expand_macro(macro_name, args)
                output.extend(self.process_lines(expanded, filename))
                i += 1
                continue

            # Regular line — substitute \&var and .equ references
            line_out = self.subst_string_vars(line)
            line_out = self.subst_equ_vars(line_out)
            output.append(line_out)
            i += 1

        return output

    def subst_equ_vars(self, line):
        for name, val in self.equ_vars.items():
            line = re.sub(r'\b' + re.escape(name) + r'\b', str(val), line)
        return line

    def subst_string_vars(self, line):
        def repl(m):
            name = m.group(1)
            if name in self.string_vars:
                return self.string_vars[name]
            return m.group(0)
        return re.sub(r'\\&(\w+)', repl, line)

    def expand_macro(self, name, args):
        params, body, defaults = self.macros[name]
        self.expand_counter += 1
        counter = self.expand_counter
        result = []
        for line in body:
            l = line
            for pi, pname in enumerate(params):
                val = args[pi] if pi < len(args) else defaults.get(pname, '')
                l = l.replace('\\' + pname, val)
            l = l.replace('\\@', str(counter))
            l = self.subst_string_vars(l)
            result.append(l)
        result = self.resolve_conditionals(result)
        return result

    def resolve_conditionals(self, lines):
        output = []
        i = 0
        while i < len(lines):
            stripped = lines[i].strip()
            m = re.match(r'\s*\.aif\s+(.*)', stripped)
            if m:
                cond = m.group(1).strip()
                true_block = []
                false_block = []
                current = true_block
                i += 1
                depth = 1
                while i < len(lines) and depth > 0:
                    s = lines[i].strip()
                    if re.match(r'\s*\.aif\s+', s):
                        depth += 1
                        current.append(lines[i])
                    elif re.match(r'\s*\.aelse', s) and depth == 1:
                        current = false_block
                    elif re.match(r'\s*\.aendi', s):
                        depth -= 1
                        if depth == 0:
                            i += 1
                            break
                        current.append(lines[i])
                    else:
                        current.append(lines[i])
                    i += 1
                if self.eval_condition(cond):
                    output.extend(true_block)
                else:
                    output.extend(false_block)
                continue
            output.append(lines[i])
            i += 1
        return output

    def eval_condition(self, cond):
        m = re.match(r'"([^"]*)"\s+(?:eq|EQ)\s+"([^"]*)"', cond)
        if m:
            return m.group(1) == m.group(2)
        return False


def main():
    import argparse
    parser = argparse.ArgumentParser(description='Minimal gasp replacement')
    parser.add_argument('input', help='Input file')
    parser.add_argument('-o', '--output', help='Output file (default: stdout)')
    parser.add_argument('-I', action='append', default=[], dest='includes', help='Include directory')
    parser.add_argument('-c', default=';', help='Comment character')
    parser.add_argument('-p', action='store_true', help='Compatibility flag (ignored)')
    parser.add_argument('-s', action='store_true', help='Compatibility flag (ignored)')
    args = parser.parse_args()

    base_dir = os.path.dirname(os.path.abspath(args.input))
    include_dirs = [base_dir] + [os.path.abspath(d) for d in args.includes]

    with open(args.input, 'r') as f:
        lines = f.readlines()

    expander = GaspExpander(include_dirs, args.c)
    output = expander.process_lines(lines, args.input)

    text = ''.join(output)
    if args.output:
        with open(args.output, 'w', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
