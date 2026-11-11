import argparse

argv = argparse.ArgumentParser()


class token:
    def __init__(self):
        self.state = "init"
        self.kind = ""
        self.start = 0
        self.len = 0
        self.line = 0
        self.col = 0

    def new(self, kind, start, length, line, col):
        self.kind = kind
        self.start = start
        self.len = length
        self.line = line
        self.col = col
        return self

    def __repr__(self):
        return f"Token({self.kind}, start={self.start}, len={self.len}, line={self.line}, col={self.col})"


class tokenizer:
    def __init__(self, b, pos, line, col, state):
        self.b = b
        self.pos = pos
        self.line = line
        self.col = col
        self.state = state

    def at_end(self) -> bool:
        return self.pos >= len(self.b)

    def peek(self):
        if self.at_end():
            return "\0"
        return self.b[self.pos]

    def consume(self):
        npos = self.pos + 1
        if npos >= len(self.b):
            return "\0"
        return self.b[npos]


def advance(t: tokenizer) -> str:
    if t.at_end():
        return "\0"

    c = t.b[t.pos]
    t.pos += 1

    if c == "\n":
        t.line += 1
        t.col = 1  # Reseta coluna para 1 na nova linha
    else:
        t.col += 1

    return c


def lexer(t: tokenizer):
    tok = token()

    # Se o arquivo já acabou antes de começar, retorna EOF imediatamente
    if t.at_end():
        t.state = "START"
        return tok.new("EOF", t.pos, 0, t.line, t.col)

    # Variáveis locais para rastrear o início exato do token atual
    start_pos = t.pos
    start_line = t.line
    start_col = t.col

    while True:
        # Se o arquivo acabar no meio de qualquer processamento
        if t.at_end():
            old_state = t.state
            t.state = "START"
            # Se o arquivo acabar enquanto acumulava um token, retorna ele antes do EOF definitivo
            if old_state in ["STRING_LIT", "NUMBER", "IDENTIFIER"]:
                return tok.new(old_state, start_pos, t.pos - start_pos, start_line, start_col)
            return tok.new("EOF", t.pos, 0, t.line, t.col)

        c = t.peek()

        # ---------------- ESTADO: START ----------------
        if t.state == "START":
            start_pos = t.pos
            start_line = t.line
            start_col = t.col

            if c == '"':
                advance(t)
                t.state = "STRING_LIT"
                continue

            if c.isdigit():
                t.state = "NUMBER"
                advance(t)
                continue

            # Detecta o comentário de linha '--'
            if c == "-" and t.consume() == "-":
                t.state = "LINE_COMMENT"
                advance(t)
                advance(t)
                continue

            # Pula espaços e quebras de linha de forma contínua
            if c.isspace() or c == "\t" or c == "\r" or c == "\n":
                advance(t)
                continue

            # Detecta identificadores (letras ou _)
            if c.isalpha() or c == "_":
                t.state = "IDENTIFIER"
                advance(t)
                continue

            # Captura símbolos individuais (como parênteses, pontos, etc.)
            advance(t)
            return tok.new("SYMBOL", start_pos, 1, start_line, start_col)

        # ---------------- ESTADO: LINE_COMMENT ----------------
        if t.state == "LINE_COMMENT":
            if c == "\n":
                advance(t)
                t.state = "START"
                continue  # Comentário acabou, volta para o START buscar o próximo token real
            
            advance(t)
            continue

        # ---------------- ESTADO: STRING_LIT ----------------
        if t.state == "STRING_LIT":
            if c == '"':  # Fecha as aspas da string
                advance(t)
                t.state = "START"
                return tok.new("STRING_LIT", start_pos, t.pos - start_pos, start_line, start_col)
            advance(t)
            continue

        # ---------------- ESTADO: NUMBER ----------------
        if t.state == "NUMBER":
            if c.isdigit():
                advance(t)
                continue
            t.state = "START"
            return tok.new("NUMBER", start_pos, t.pos - start_pos, start_line, start_col)

        # ---------------- ESTADO: IDENTIFIER ----------------
        if t.state == "IDENTIFIER":
            if c.isalnum() or c == "_":
                advance(t)
                continue
            t.state = "START"
            return tok.new("IDENTIFIER", start_pos, t.pos - start_pos, start_line, start_col)


def main():
    argv.add_argument("source", help="program entrypoint")
    args = argv.parse_args()

    with open(args.source, "rb") as file:
        raw = file.read()
        entrypoint = raw.decode("utf-8")
        t = tokenizer(
            b=entrypoint,
            pos=0,
            line=1,
            col=1,
            state="START",
        )

        # Executa o loop lendo token por token de forma linear
        while True:
            tok = lexer(t)
            print(tok)
            if tok.kind == "EOF":
                break


if __name__ == "__main__":
    main()

