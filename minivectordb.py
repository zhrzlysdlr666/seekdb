#!/usr/bin/env python3
"""MiniVectorDB - a compact SQL + vector engine speaking the round-2 JSONL stdio protocol.

Design
------
* Durable state lives under the data directory as an append-only WAL of
  committed transactions plus periodic full snapshots. Every commit is fsynced
  before the response is written, so data committed before `docker kill -9`
  survives the restart.
* Transactions stage changes in memory and record an undo journal. A failing
  statement rolls back only its own changes, so earlier statements of the same
  explicit transaction stay intact and can still be committed.
* Vectors are quantised to float32 on write, matching the reference engine's
  storage semantics (16777217 and 16777216 are the same float32 value).
* No third-party dependency: the image only needs a Python interpreter.
"""

from __future__ import annotations

import json
import math
import os
import struct
import sys
from pathlib import Path

RECOVERY_VERSION = 1
FLOAT32_MAX = 3.4028234663852886e38
WAL_NAME = "wal.jsonl"
SNAP_NAME = "snapshot.json"
CHECKPOINT_BYTES = 1 << 20


# --------------------------------------------------------------------------- #
# errors
# --------------------------------------------------------------------------- #
class SQLError(Exception):
    """A statement-level failure carrying a stable machine-readable code."""

    def __init__(self, code: str, message: str = "") -> None:
        self.code = code
        self.message = message or code
        super().__init__(self.message)


def fail(code: str, message: str = "") -> "SQLError":
    return SQLError(code, message)


# --------------------------------------------------------------------------- #
# value helpers
# --------------------------------------------------------------------------- #
def quantise_float32(value):
    """Round a Python float to the nearest float32 value, rejecting non-finite input."""

    if isinstance(value, bool):
        raise fail("invalid_vector", "boolean is not a vector component")
    if not isinstance(value, (int, float)):
        raise fail("invalid_vector", "vector component must be numeric")
    number = float(value)
    if math.isnan(number):
        raise fail("invalid_vector", "NaN is not a valid vector component")
    if math.isinf(number):
        raise fail("invalid_vector", "Infinity is not a valid vector component")
    if abs(number) > FLOAT32_MAX:
        raise fail("float32_overflow", "vector component exceeds float32 range")
    try:
        packed = struct.pack("<f", number)
    except (OverflowError, struct.error):
        raise fail("float32_overflow", "vector component exceeds float32 range") from None
    narrowed = struct.unpack("<f", packed)[0]
    if math.isinf(narrowed):
        raise fail("float32_overflow", "vector component exceeds float32 range")
    return narrowed


def parse_vector_text(text, dim=None):
    """Parse a vector literal such as ``'[3,4]'`` into a float32 tuple."""

    if isinstance(text, (list, tuple)):
        raw = list(text)
    else:
        body = str(text).strip()
        if not (body.startswith("[") and body.endswith("]")):
            raise fail("invalid_vector", "vector literal must look like [a,b]")
        body = body[1:-1].strip()
        if not body:
            raw = []
        else:
            raw = []
            for part in body.split(","):
                token = part.strip()
                if not token:
                    raise fail("invalid_vector", "empty vector component")
                try:
                    raw.append(float(token))
                except ValueError:
                    raise fail("invalid_vector", f"vector component {token!r} is not a number") from None
    values = tuple(quantise_float32(item) for item in raw)
    if dim is not None and len(values) != dim:
        raise fail("dimension_mismatch", f"expected {dim} dimensions, got {len(values)}")
    return values


def as_int(value):
    if isinstance(value, bool) or not isinstance(value, int):
        raise fail("type_mismatch", "expected an integer")
    return value


def json_safe(value):
    """Return a JSON-serialisable projection that keeps integers exact."""

    if isinstance(value, tuple):
        return [json_safe(item) for item in value]
    if isinstance(value, list):
        return [json_safe(item) for item in value]
    if isinstance(value, float):
        if not math.isfinite(value):
            raise fail("invalid_vector", "non-finite value cannot be returned")
        return value
    return value


# --------------------------------------------------------------------------- #
# lexer
# --------------------------------------------------------------------------- #
KEYWORDS = {
    "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE",
    "CREATE", "TABLE", "DROP", "INDEX", "VECTOR", "ON", "AND", "OR", "NOT", "NULL",
    "ORDER", "BY", "ASC", "DESC", "LIMIT", "OFFSET", "BEGIN", "COMMIT", "ROLLBACK",
    "START", "TRANSACTION", "PRIMARY", "KEY", "AS", "DISTINCT", "IN", "LIKE", "IS",
    "GROUP", "COUNT", "SUM", "AVG", "MIN", "MAX", "IF", "EXISTS", "TRUE", "FALSE",
    "UNIQUE", "DEFAULT", "TEXT", "BIGINT", "INT", "INTEGER", "VARCHAR", "CHAR",
}

PUNCT = {
    "(", ")", ",", ";", "=", "!=", "<>", "<", "<=", ">", ">=", "*", ".", "+", "-", "/",
}

SIMPLE_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", "'": "'", '"': '"'}


class Token:
    __slots__ = ("kind", "text", "value")

    def __init__(self, kind, text, value=None):
        self.kind = kind
        self.text = text
        self.value = value

    def __repr__(self) -> str:  # pragma: no cover - debug aid
        return f"Token({self.kind},{self.text!r})"


def tokenize(sql: str):
    tokens = []
    index = 0
    length = len(sql)
    while index < length:
        char = sql[index]
        if char in " \t\r\n":
            index += 1
            continue
        if char == "-" and sql.startswith("--", index):
            newline = sql.find("\n", index)
            index = length if newline < 0 else newline + 1
            continue
        if char == "#":
            newline = sql.find("\n", index)
            index = length if newline < 0 else newline + 1
            continue
        if char == "/" and sql.startswith("/*", index):
            end = sql.find("*/", index + 2)
            if end < 0:
                raise fail("syntax_error", "unterminated comment")
            index = end + 2
            continue
        if char in "'\"":
            quote = char
            index += 1
            buffer = []
            while True:
                if index >= length:
                    raise fail("syntax_error", "unterminated string literal")
                current = sql[index]
                if current == "\\" and index + 1 < length:
                    nxt = sql[index + 1]
                    buffer.append(SIMPLE_ESCAPES.get(nxt, nxt))
                    index += 2
                    continue
                if current == quote:
                    if index + 1 < length and sql[index + 1] == quote:
                        buffer.append(quote)
                        index += 2
                        continue
                    index += 1
                    break
                buffer.append(current)
                index += 1
            tokens.append(Token("string", "".join(buffer), "".join(buffer)))
            continue
        if char.isdigit() or (char == "." and index + 1 < length and sql[index + 1].isdigit()):
            start = index
            seen_dot = False
            seen_exp = False
            while index < length:
                current = sql[index]
                if current.isdigit():
                    index += 1
                    continue
                if current == "." and not seen_dot and not seen_exp:
                    seen_dot = True
                    index += 1
                    continue
                if current in "eE" and not seen_exp:
                    seen_exp = True
                    index += 1
                    if index < length and sql[index] in "+-":
                        index += 1
                    continue
                break
            text = sql[start:index]
            if seen_dot or seen_exp:
                tokens.append(Token("number", text, float(text)))
            else:
                tokens.append(Token("number", text, int(text)))
            continue
        if char.isalpha() or char == "_":
            start = index
            while index < length and (sql[index].isalnum() or sql[index] in "_$"):
                index += 1
            text = sql[start:index]
            upper = text.upper()
            tokens.append(Token("keyword" if upper in KEYWORDS else "ident", text, upper))
            continue
        two = sql[index:index + 2]
        if two in ("!=", "<>", "<=", ">="):
            tokens.append(Token("punct", two, two))
            index += 2
            continue
        if char in PUNCT:
            tokens.append(Token("punct", char, char))
            index += 1
            continue
        raise fail("syntax_error", f"unexpected character {char!r}")
    tokens.append(Token("eof", "", None))
    return tokens


# --------------------------------------------------------------------------- #
# parser -> tiny AST (tuples)
# --------------------------------------------------------------------------- #
COMPARISONS = {"=", "!=", "<>", "<", "<=", ">", ">="}


class Parser:
    def __init__(self, sql: str) -> None:
        self.sql = sql
        self.tokens = tokenize(sql)
        self.position = 0

    # -- token helpers ---------------------------------------------------- #
    def peek(self, offset: int = 0) -> Token:
        index = min(self.position + offset, len(self.tokens) - 1)
        return self.tokens[index]

    def next(self) -> Token:
        token = self.tokens[self.position]
        if token.kind != "eof":
            self.position += 1
        return token

    def at_keyword(self, *names: str) -> bool:
        token = self.peek()
        return token.kind == "keyword" and token.value in names

    def accept_keyword(self, *names: str) -> bool:
        if self.at_keyword(*names):
            self.next()
            return True
        return False

    def expect_keyword(self, name: str) -> Token:
        if not self.at_keyword(name):
            raise fail("syntax_error", f"expected {name}, got {self.peek().text or 'end of statement'}")
        return self.next()

    def at_punct(self, *symbols: str) -> bool:
        token = self.peek()
        return token.kind == "punct" and token.value in symbols

    def accept_punct(self, *symbols: str) -> bool:
        if self.at_punct(*symbols):
            self.next()
            return True
        return False

    def expect_punct(self, symbol: str) -> Token:
        if not self.at_punct(symbol):
            raise fail("syntax_error", f"expected {symbol}, got {self.peek().text or 'end of statement'}")
        return self.next()

    def identifier(self) -> str:
        token = self.peek()
        if token.kind == "ident":
            self.next()
            return token.text
        if token.kind == "keyword":
            self.next()
            return token.text
        raise fail("syntax_error", f"expected an identifier, got {token.text or 'end of statement'}")

    # -- statement -------------------------------------------------------- #
    def parse_statement(self):
        while self.accept_punct(";"):
            pass
        token = self.peek()
        if token.kind == "eof":
            return ("noop",)
        if token.kind == "keyword":
            handler = {
                "SELECT": self.parse_select,
                "INSERT": self.parse_insert,
                "UPDATE": self.parse_update,
                "DELETE": self.parse_delete,
                "CREATE": self.parse_create,
                "DROP": self.parse_drop,
                "BEGIN": self.parse_begin,
                "START": self.parse_begin,
                "COMMIT": self.parse_commit,
                "ROLLBACK": self.parse_rollback,
                "ALTER": self.parse_unsupported,
                "TRUNCATE": self.parse_unsupported,
                "RENAME": self.parse_unsupported,
                "GRANT": self.parse_unsupported,
                "SET": self.parse_unsupported,
                "USE": self.parse_unsupported,
                "SHOW": self.parse_show,
                "DESC": self.parse_show,
                "DESCRIBE": self.parse_show,
                "EXPLAIN": self.parse_show,
            }.get(token.value)
            if handler is not None:
                node = handler()
                while self.accept_punct(";"):
                    pass
                return node
        raise fail("unsupported_sql", f"unsupported statement near {token.text!r}")

    def parse_unsupported(self):
        verb = self.next().text
        raise fail("unsupported_sql", f"{verb.upper()} is not supported")

    def parse_begin(self):
        self.next()
        self.accept_keyword("TRANSACTION")
        return ("begin",)

    def parse_commit(self):
        self.next()
        return ("commit",)

    def parse_rollback(self):
        self.next()
        return ("rollback",)

    def parse_show(self):
        verb = self.next().value
        if verb in ("DESC", "DESCRIBE", "EXPLAIN"):
            table = self.identifier()
            return ("describe", table)
        if self.accept_keyword("TABLES"):
            return ("show_tables",)
        raise fail("unsupported_sql", "SHOW is only supported for TABLES")

    def parse_create(self):
        self.expect_keyword("CREATE")
        if self.accept_keyword("VECTOR"):
            self.expect_keyword("INDEX")
            name = self.identifier()
            self.expect_keyword("ON")
            table = self.identifier()
            self.expect_punct("(")
            column = self.identifier()
            self.expect_punct(")")
            return ("create_index", table, name, column)
        if self.accept_keyword("INDEX"):
            name = self.identifier()
            self.expect_keyword("ON")
            table = self.identifier()
            self.expect_punct("(")
            column = self.identifier()
            self.expect_punct(")")
            return ("create_index", table, name, column)
        if self.accept_keyword("TABLE"):
            if_not_exists = False
            if self.at_keyword("IF"):
                self.next()
                self.expect_keyword("NOT")
                self.expect_keyword("EXISTS")
                if_not_exists = True
            name = self.identifier()
            self.expect_punct("(")
            columns = []
            while True:
                columns.append(self.parse_column())
                if self.accept_punct(","):
                    continue
                break
            self.expect_punct(")")
            if not columns:
                raise fail("syntax_error", "table needs at least one column")
            return ("create_table", name, columns, if_not_exists)
        raise fail("unsupported_sql", "unsupported CREATE object")

    def parse_column(self):
        name = self.identifier()
        token = self.peek()
        if token.kind not in ("keyword", "ident"):
            raise fail("syntax_error", f"missing type for column {name}")
        self.next()
        base = token.value if token.kind == "keyword" else token.text.upper()
        length = None
        dimension = None
        if base in ("VARCHAR", "CHAR"):
            if self.accept_punct("("):
                length = as_int(self.next().value)
                self.expect_punct(")")
        elif base == "VECTOR":
            if not self.accept_punct("("):
                raise fail("syntax_error", "VECTOR requires a dimension")
            dimension = as_int(self.next().value)
            self.expect_punct(")")
        elif base in ("INT", "INTEGER"):
            base = "BIGINT"
        elif base in ("TEXT", "BIGINT"):
            pass
        else:
            raise fail("unsupported_sql", f"unsupported column type {base}")
        primary = False
        while True:
            if self.accept_keyword("PRIMARY"):
                self.expect_keyword("KEY")
                primary = True
                continue
            if self.accept_keyword("NOT"):
                self.expect_keyword("NULL")
                continue
            if self.accept_keyword("UNIQUE"):
                continue
            if self.accept_keyword("DEFAULT"):
                self.next()
                continue
            break
        return {"name": name, "type": base, "length": length, "dim": dimension, "primary": primary}

    def parse_drop(self):
        self.expect_keyword("DROP")
        if self.accept_keyword("TABLE"):
            if_exists = False
            if self.at_keyword("IF"):
                self.next()
                self.expect_keyword("EXISTS")
                if_exists = True
            return ("drop_table", self.identifier(), if_exists)
        if self.accept_keyword("INDEX"):
            name = self.identifier()
            self.expect_keyword("ON")
            return ("drop_index", self.identifier(), name)
        raise fail("unsupported_sql", "unsupported DROP object")

    def parse_insert(self):
        self.expect_keyword("INSERT")
        self.expect_keyword("INTO")
        table = self.identifier()
        columns = None
        if self.at_punct("("):
            self.next()
            columns = []
            while True:
                columns.append(self.identifier())
                if self.accept_punct(","):
                    continue
                break
            self.expect_punct(")")
        self.expect_keyword("VALUES")
        rows = []
        while True:
            self.expect_punct("(")
            values = []
            while True:
                values.append(self.parse_expr())
                if self.accept_punct(","):
                    continue
                break
            self.expect_punct(")")
            rows.append(values)
            if self.accept_punct(","):
                continue
            break
        return ("insert", table, columns, rows)

    def parse_update(self):
        self.expect_keyword("UPDATE")
        table = self.identifier()
        self.expect_keyword("SET")
        assignments = []
        while True:
            column = self.identifier()
            self.expect_punct("=")
            assignments.append((column, self.parse_expr()))
            if self.accept_punct(","):
                continue
            break
        where = None
        if self.accept_keyword("WHERE"):
            where = self.parse_expr()
        return ("update", table, assignments, where)

    def parse_delete(self):
        self.expect_keyword("DELETE")
        self.expect_keyword("FROM")
        table = self.identifier()
        where = None
        if self.accept_keyword("WHERE"):
            where = self.parse_expr()
        return ("delete", table, where)

    def parse_select(self):
        self.expect_keyword("SELECT")
        distinct = self.accept_keyword("DISTINCT")
        items = []
        while True:
            if self.at_punct("*"):
                self.next()
                items.append(("*", None))
            else:
                expr = self.parse_expr()
                alias = None
                if self.accept_keyword("AS"):
                    alias = self.identifier()
                elif self.peek().kind == "ident":
                    alias = self.next().text
                items.append((expr, alias))
            if self.accept_punct(","):
                continue
            break
        table = None
        if self.accept_keyword("FROM"):
            table = self.identifier()
            if self.at_punct(","):
                raise fail("unsupported_sql", "joins are not supported")
        where = None
        if self.accept_keyword("WHERE"):
            where = self.parse_expr()
        group = []
        if self.accept_keyword("GROUP"):
            self.expect_keyword("BY")
            while True:
                group.append(self.parse_expr())
                if self.accept_punct(","):
                    continue
                break
        order = []
        if self.accept_keyword("ORDER"):
            self.expect_keyword("BY")
            while True:
                expr = self.parse_expr()
                descending = False
                if self.accept_keyword("DESC"):
                    descending = True
                else:
                    self.accept_keyword("ASC")
                order.append((expr, descending))
                if self.accept_punct(","):
                    continue
                break
        limit = None
        offset = 0
        if self.accept_keyword("LIMIT"):
            first = self.next()
            if first.kind != "number" or not isinstance(first.value, int):
                raise fail("syntax_error", "LIMIT needs an integer")
            limit = first.value
            if self.accept_punct(","):
                second = self.next()
                if second.kind != "number" or not isinstance(second.value, int):
                    raise fail("syntax_error", "LIMIT needs an integer")
                offset, limit = limit, second.value
            elif self.accept_keyword("OFFSET"):
                offset_token = self.next()
                if offset_token.kind != "number" or not isinstance(offset_token.value, int):
                    raise fail("syntax_error", "OFFSET needs an integer")
                offset = offset_token.value
        return ("select", distinct, items, table, where, group, order, limit, offset)

    # -- expressions ------------------------------------------------------ #
    def parse_expr(self):
        return self.parse_or()

    def parse_or(self):
        node = self.parse_and()
        while self.accept_keyword("OR"):
            node = ("or", node, self.parse_and())
        return node

    def parse_and(self):
        node = self.parse_not()
        while self.accept_keyword("AND"):
            node = ("and", node, self.parse_not())
        return node

    def parse_not(self):
        if self.accept_keyword("NOT"):
            return ("not", self.parse_not())
        return self.parse_comparison()

    def parse_comparison(self):
        node = self.parse_additive()
        while True:
            token = self.peek()
            if token.kind == "punct" and token.value in COMPARISONS:
                self.next()
                operator = "!=" if token.value == "<>" else token.value
                node = ("compare", operator, node, self.parse_additive())
                continue
            if self.accept_keyword("IS"):
                negated = self.accept_keyword("NOT")
                self.expect_keyword("NULL")
                node = ("isnull", node, negated)
                continue
            if self.accept_keyword("IN"):
                self.expect_punct("(")
                values = []
                if not self.at_punct(")"):
                    while True:
                        values.append(self.parse_expr())
                        if self.accept_punct(","):
                            continue
                        break
                self.expect_punct(")")
                node = ("in", node, values)
                continue
            if self.accept_keyword("LIKE"):
                node = ("like", node, self.parse_additive())
                continue
            break
        return node

    def parse_additive(self):
        node = self.parse_multiplicative()
        while self.at_punct("+", "-"):
            operator = self.next().value
            node = ("arith", operator, node, self.parse_multiplicative())
        return node

    def parse_multiplicative(self):
        node = self.parse_unary()
        while self.at_punct("*", "/"):
            operator = self.next().value
            node = ("arith", operator, node, self.parse_unary())
        return node

    def parse_unary(self):
        if self.accept_punct("-"):
            return ("arith", "-", ("literal", 0), self.parse_unary())
        if self.accept_punct("+"):
            return self.parse_unary()
        return self.parse_primary()

    def parse_primary(self):
        token = self.peek()
        if token.kind == "number":
            self.next()
            return ("literal", token.value)
        if token.kind == "string":
            self.next()
            return ("literal", token.value)
        if token.kind == "keyword" and token.value in ("NULL", "TRUE", "FALSE"):
            self.next()
            return ("literal", {"NULL": None, "TRUE": True, "FALSE": False}[token.value])
        if self.accept_punct("("):
            if self.at_keyword("SELECT"):
                raise fail("unsupported_sql", "subqueries are not supported")
            node = self.parse_expr()
            self.expect_punct(")")
            return node
        if token.kind in ("ident", "keyword"):
            name = self.next().text
            if self.at_punct("("):
                self.next()
                arguments = []
                if self.at_punct("*"):
                    self.next()
                    arguments.append(("*", None))
                elif not self.at_punct(")"):
                    while True:
                        arguments.append(self.parse_expr())
                        if self.accept_punct(","):
                            continue
                        break
                self.expect_punct(")")
                return ("call", name.upper(), arguments)
            if self.accept_punct("."):
                column = self.identifier()
                return ("column", column)
            return ("column", name)
        raise fail("syntax_error", f"unexpected token {token.text or 'end of statement'}")


# --------------------------------------------------------------------------- #
# storage
# --------------------------------------------------------------------------- #
class Store:
    """Durable state: snapshot + append-only WAL of committed transactions."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.root.mkdir(parents=True, exist_ok=True)
        self.snapshot_path = self.root / SNAP_NAME
        self.wal_path = self.root / WAL_NAME
        self.wal_bytes = self.wal_path.stat().st_size if self.wal_path.exists() else 0

    def load(self):
        tables = {}
        seq = 0
        if self.snapshot_path.exists():
            try:
                payload = json.loads(self.snapshot_path.read_text(encoding="utf-8"))
            except (ValueError, OSError):
                payload = None
            if isinstance(payload, dict):
                seq = int(payload.get("seq") or 0)
                for name, body in (payload.get("tables") or {}).items():
                    tables[name] = {
                        "columns": body.get("columns") or [],
                        "rows": [[row[0], list(row[1])] for row in body.get("rows") or []],
                        "indexes": body.get("indexes") or {},
                    }
        if self.wal_path.exists():
            seq = self._replay(tables, seq)
        return tables, seq

    def _replay(self, tables, seq):
        data = self.wal_path.read_bytes()
        position = 0
        good_end = 0
        length = len(data)
        while position < length:
            newline = data.find(b"\n", position)
            if newline < 0:
                break
            chunk = data[position:newline]
            try:
                record = json.loads(chunk.decode("utf-8"))
            except (ValueError, UnicodeError):
                break
            if not isinstance(record, dict):
                break
            self._apply(tables, record)
            seq = max(seq, int(record.get("seq") or 0))
            position = newline + 1
            good_end = position
        if good_end != length:
            # Torn tail from a crash: drop it so new appends stay readable.
            with open(self.wal_path, "r+b") as handle:
                handle.truncate(good_end)
                handle.flush()
                os.fsync(handle.fileno())
            self.wal_bytes = good_end
        return seq

    @staticmethod
    def _apply(tables, record):
        for op in record.get("ops") or []:
            kind = op.get("op")
            name = op.get("table")
            if kind == "create_table":
                tables[name] = {"columns": op["columns"], "rows": [], "indexes": {}}
            elif kind == "drop_table":
                tables.pop(name, None)
            elif kind == "create_index":
                table = tables.get(name)
                if table is not None:
                    table["indexes"][op["name"]] = op["column"]
            elif kind == "put":
                table = tables.get(name)
                if table is None:
                    continue
                rows = table["rows"]
                for entry in rows:
                    if entry[0] == op["pk"]:
                        entry[1] = list(op["row"])
                        break
                else:
                    rows.append([op["pk"], list(op["row"])])
            elif kind == "remove":
                table = tables.get(name)
                if table is None:
                    continue
                table["rows"] = [entry for entry in table["rows"] if entry[0] != op["pk"]]

    def append(self, record) -> None:
        line = json.dumps(record, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
        payload = line.encode("utf-8") + b"\n"
        with open(self.wal_path, "ab") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        self.wal_bytes += len(payload)

    def write_snapshot(self, tables, seq) -> None:
        body = {
            "version": RECOVERY_VERSION,
            "seq": seq,
            "tables": {
                name: {
                    "columns": table["columns"],
                    "rows": [[entry[0], list(entry[1])] for entry in table["rows"]],
                    "indexes": table["indexes"],
                }
                for name, table in tables.items()
            },
        }
        temporary = self.snapshot_path.with_suffix(".json.tmp")
        with open(temporary, "wb") as handle:
            handle.write(json.dumps(body, ensure_ascii=False, allow_nan=False).encode("utf-8"))
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, self.snapshot_path)
        self._fsync_dir()
        with open(self.wal_path, "wb") as handle:
            handle.truncate(0)
            handle.flush()
            os.fsync(handle.fileno())
        self.wal_bytes = 0

    def _fsync_dir(self) -> None:
        try:
            descriptor = os.open(self.root, os.O_RDONLY)
        except OSError:
            return
        try:
            os.fsync(descriptor)
        except OSError:
            pass
        finally:
            os.close(descriptor)


# --------------------------------------------------------------------------- #
# engine
# --------------------------------------------------------------------------- #
AGGREGATES = {"COUNT", "SUM", "AVG", "MIN", "MAX"}


def _is_aggregate(node) -> bool:
    return isinstance(node, tuple) and node and node[0] == "call" and node[1] in AGGREGATES


def _has_aggregate(node) -> bool:
    if isinstance(node, tuple):
        if _is_aggregate(node):
            return True
        return any(_has_aggregate(item) for item in node[1:] if isinstance(item, (tuple, list)))
    if isinstance(node, list):
        return any(_has_aggregate(item) for item in node)
    return False


class Engine:
    def __init__(self, data_dir: Path) -> None:
        self.store = Store(data_dir)
        self.raw_tables, self.seq = self.store.load()
        self.tables = {}
        for name, body in self.raw_tables.items():
            rows = {}
            order = []
            for pk, values in body["rows"]:
                rows[pk] = list(values)
                order.append(pk)
            self.tables[name] = {
                "columns": body["columns"],
                "rows": rows,
                "order": order,
                "indexes": body["indexes"],
            }
        self.transaction = None
        self.committed_ops = []

    # -- lifecycle -------------------------------------------------------- #
    def close(self) -> None:
        # Uncommitted work never survives shutdown, so drop it before the
        # checkpoint writes the surviving state to disk.
        if self.transaction is not None:
            self.rollback()
        self.committed_ops = []
        self.checkpoint(force=True)

    def checkpoint(self, force: bool = False) -> None:
        if not force and self.store.wal_bytes < CHECKPOINT_BYTES:
            return
        tables = {
            name: {
                "columns": table["columns"],
                "rows": [[pk, table["rows"][pk]] for pk in table["order"] if pk in table["rows"]],
                "indexes": table["indexes"],
            }
            for name, table in self.tables.items()
        }
        self.store.write_snapshot(tables, self.seq)

    # -- transaction plumbing --------------------------------------------- #
    def _stage(self, op) -> None:
        if self.transaction is None:
            self.committed_ops.append(op)
        else:
            self.transaction["ops"].append(op)

    def begin(self) -> None:
        if self.transaction is not None:
            raise fail("transaction_active", "a transaction is already open")
        self.transaction = {"ops": [], "undo": []}

    def commit(self) -> None:
        if self.transaction is None:
            return
        ops = self.transaction["ops"]
        self.transaction = None
        if not ops:
            return
        self.seq += 1
        self.store.append({"seq": self.seq, "ops": ops})
        self.checkpoint()

    def rollback(self) -> None:
        if self.transaction is None:
            return
        for entry in reversed(self.transaction["undo"]):
            self._restore(entry)
        self.transaction = None

    def _restore(self, entry) -> None:
        table_name, pk, before = entry
        table = self.tables.get(table_name)
        if table is None:
            return
        if before is None:
            if pk in table["rows"]:
                del table["rows"][pk]
                try:
                    table["order"].remove(pk)
                except ValueError:
                    pass
        else:
            if pk not in table["rows"]:
                table["order"].append(pk)
            table["rows"][pk] = list(before)

    def _autocommit(self) -> None:
        if self.transaction is None and self.committed_ops:
            ops = self.committed_ops
            self.committed_ops = []
            self.seq += 1
            self.store.append({"seq": self.seq, "ops": ops})
            self.checkpoint()

    # -- statement entry -------------------------------------------------- #
    def execute(self, sql: str, mode: str = "exact"):
        node = Parser(sql).parse_statement()
        kind = node[0]
        if kind == "noop":
            return {}
        if kind == "begin":
            self.begin()
            return {}
        if kind == "commit":
            self.commit()
            return {}
        if kind == "rollback":
            self.rollback()
            return {}
        if kind == "select":
            return self._select(node)
        if kind == "show_tables":
            names = sorted(self.tables)
            return {"columns": ["table"], "rows": [[name] for name in names]}
        if kind == "describe":
            return self._describe(node[1])
        undo_mark = len(self.transaction["undo"]) if self.transaction else 0
        try:
            if kind == "create_table":
                result = self._create_table(*node[1:])
            elif kind == "drop_table":
                result = self._drop_table(*node[1:])
            elif kind == "create_index":
                result = self._create_index(*node[1:])
            elif kind == "drop_index":
                result = self._drop_index(*node[1:])
            elif kind == "insert":
                result = self._insert(*node[1:], mode=mode)
            elif kind == "update":
                result = self._update(*node[1:])
            elif kind == "delete":
                result = self._delete(*node[1:])
            else:  # pragma: no cover - defensive
                raise fail("unsupported_sql", f"unsupported statement {kind}")
        except Exception:
            if self.transaction is not None:
                while len(self.transaction["undo"]) > undo_mark:
                    self._restore(self.transaction["undo"].pop())
            self.committed_ops = []
            raise
        self._autocommit()
        return result

    # -- schema ----------------------------------------------------------- #
    def _table(self, name: str):
        table = self.tables.get(name)
        if table is None:
            raise fail("unknown_table", f"table {name} does not exist")
        return table

    def _column_index(self, table, name: str) -> int:
        for index, column in enumerate(table["columns"]):
            if column["name"].lower() == name.lower():
                return index
        raise fail("unknown_column", f"column {name} does not exist")

    def _primary_index(self, table):
        for index, column in enumerate(table["columns"]):
            if column.get("primary"):
                return index
        return None

    def _describe(self, name: str):
        table = self._table(name)
        rows = []
        for column in table["columns"]:
            kind = column["type"]
            if kind == "VARCHAR" and column.get("length"):
                kind = f"VARCHAR({column['length']})"
            elif kind == "VECTOR" and column.get("dim"):
                kind = f"VECTOR({column['dim']})"
            rows.append([column["name"], kind])
        return {"columns": ["column", "type"], "rows": rows}

    def _create_table(self, name, columns, if_not_exists):
        if name in self.tables and if_not_exists:
            return {}
        if name in self.tables:
            raise fail("table_exists", f"table {name} already exists")
        primaries = [column for column in columns if column["primary"]]
        if len(primaries) > 1:
            raise fail("unsupported_sql", "composite primary keys are not supported")
        self.tables[name] = {"columns": columns, "rows": {}, "order": [], "indexes": {}}
        self._stage({"op": "create_table", "table": name, "columns": columns})
        return {}

    def _drop_table(self, name, if_exists):
        if name not in self.tables:
            if if_exists:
                return {}
            raise fail("unknown_table", f"table {name} does not exist")
        for pk in list(self.tables[name]["rows"]):
            if self.transaction is not None:
                self.transaction["undo"].append((name, pk, self.tables[name]["rows"][pk]))
        del self.tables[name]
        self._stage({"op": "drop_table", "table": name})
        return {}

    def _create_index(self, table_name, index_name, column_name):
        table = self._table(table_name)
        column_index = self._column_index(table, column_name)
        if table["columns"][column_index]["type"] != "VECTOR":
            raise fail("unsupported_sql", "only vector columns can be indexed")
        table["indexes"][index_name] = table["columns"][column_index]["name"]
        self._stage({"op": "create_index", "table": table_name, "name": index_name,
                     "column": table["columns"][column_index]["name"]})
        return {}

    def _drop_index(self, table_name, index_name):
        table = self._table(table_name)
        if index_name not in table["indexes"]:
            raise fail("unknown_index", f"index {index_name} does not exist")
        del table["indexes"][index_name]
        return {}

    # -- row coercion ----------------------------------------------------- #
    def _coerce(self, table, index, value, column_name=None):
        column = table["columns"][index]
        label = column_name or column["name"]
        if value is None:
            raise fail("not_null_violation", f"column {label} does not accept NULL")
        kind = column["type"]
        if kind == "BIGINT":
            if isinstance(value, bool):
                raise fail("type_mismatch", f"column {label} needs an integer")
            if isinstance(value, int):
                return value
            if isinstance(value, float):
                if value != int(value):
                    raise fail("type_mismatch", f"column {label} needs an integer")
                return int(value)
            text = str(value).strip()
            try:
                return int(text)
            except ValueError:
                raise fail("type_mismatch", f"column {label} needs an integer") from None
        if kind in ("VARCHAR", "CHAR", "TEXT"):
            text = value if isinstance(value, str) else str(value)
            limit = column.get("length")
            if limit is not None and len(text) > limit:
                raise fail("value_too_long", f"column {label} allows {limit} characters")
            return text
        if kind == "VECTOR":
            return parse_vector_text(value, column.get("dim"))
        raise fail("unsupported_sql", f"unsupported column type {kind}")

    # -- DML -------------------------------------------------------------- #
    def _insert(self, table_name, columns, rows, mode="exact"):
        table = self._table(table_name)
        if columns is not None:
            indexes = [self._column_index(table, name) for name in columns]
            if len(set(indexes)) != len(indexes):
                raise fail("syntax_error", "duplicate column in INSERT list")
        else:
            indexes = list(range(len(table["columns"])))
        primary = self._primary_index(table)
        prepared = []
        for row in rows:
            if len(row) != len(indexes):
                raise fail("column_count_mismatch", "value count does not match column count")
            values = [None] * len(table["columns"])
            for slot, node in zip(indexes, row):
                raw = self._eval(node, {})
                values[slot] = self._coerce(table, slot, raw)
            for index, column in enumerate(table["columns"]):
                if values[index] is None:
                    raise fail("not_null_violation", f"column {column['name']} needs a value")
            prepared.append(values)
        seen = set()
        for values in prepared:
            if primary is not None:
                key = values[primary]
                if key in seen:
                    raise fail("duplicate_key", "duplicate primary key")
                seen.add(key)
                if key in table["rows"]:
                    raise fail("duplicate_key", "duplicate primary key")
            else:
                key = len(table["order"]) + len(self.tables[table_name]["order"])
                while key in table["rows"]:
                    key += 1
            if self.transaction is not None:
                self.transaction["undo"].append(
                    (table_name, key, table["rows"].get(key))
                )
            if key not in table["rows"]:
                table["order"].append(key)
            table["rows"][key] = values
            self._stage({"op": "put", "table": table_name, "pk": key, "row": values})
        return {"affected_rows": len(prepared)}

    def _update(self, table_name, assignments, where):
        table = self._table(table_name)
        targets = []
        for column_name, _ in assignments:
            targets.append(self._column_index(table, column_name))
        if len(set(targets)) != len(targets):
            raise fail("syntax_error", "duplicate column in SET list")
        primary = self._primary_index(table)
        affected = 0
        for key in list(table["order"]):
            row = table["rows"].get(key)
            if row is None:
                continue
            context = {"row": row, "table": table}
            if where is not None and not self._truthy(self._eval(where, context)):
                continue
            new_row = list(row)
            for index, (_, node) in zip(targets, assignments):
                new_row[index] = self._coerce(table, index, self._eval(node, context))
            if primary is not None and new_row[primary] != key:
                replacement = new_row[primary]
                if replacement in table["rows"] and replacement != key:
                    raise fail("duplicate_key", "duplicate primary key")
                if self.transaction is not None:
                    self.transaction["undo"].append((table_name, key, list(row)))
                del table["rows"][key]
                table["order"][table["order"].index(key)] = replacement
                table["rows"][replacement] = new_row
                self._stage({"op": "remove", "table": table_name, "pk": key})
                self._stage({"op": "put", "table": table_name, "pk": replacement, "row": new_row})
            else:
                if self.transaction is not None:
                    self.transaction["undo"].append((table_name, key, list(row)))
                table["rows"][key] = new_row
                self._stage({"op": "put", "table": table_name, "pk": key, "row": new_row})
            affected += 1
        return {"affected_rows": affected}

    def _delete(self, table_name, where):
        table = self._table(table_name)
        affected = 0
        for key in list(table["order"]):
            row = table["rows"].get(key)
            if row is None:
                continue
            if where is not None and not self._truthy(self._eval(where, {"row": row, "table": table})):
                continue
            if self.transaction is not None:
                self.transaction["undo"].append((table_name, key, list(row)))
            del table["rows"][key]
            try:
                table["order"].remove(key)
            except ValueError:
                pass
            self._stage({"op": "remove", "table": table_name, "pk": key})
            affected += 1
        return {"affected_rows": affected}

    # -- SELECT ----------------------------------------------------------- #
    def _select(self, node):
        _, distinct, items, table_name, where, group, order, limit, offset = node
        table = self._table(table_name) if table_name else None
        source = []
        if table is not None:
            for key in table["order"]:
                row = table["rows"].get(key)
                if row is None:
                    continue
                if where is not None and not self._truthy(self._eval(where, {"row": row, "table": table})):
                    continue
                source.append({"row": row, "table": table, "pk": key})
        else:
            source.append({"row": [], "table": None, "pk": 0})

        aggregated = any(_has_aggregate(item[0]) for item in items if item[0] != "*")
        if group or aggregated:
            return self._select_aggregate(items, source, group, order, limit, offset)

        columns = []
        projections = []
        alias_map = {}
        for expr, alias in items:
            if expr == "*":
                if table is None:
                    raise fail("syntax_error", "SELECT * needs a FROM table")
                for column in table["columns"]:
                    slot = len(projections)
                    alias_map.setdefault(column["name"].lower(), slot)
                    columns.append(column["name"])
                    projections.append(("column", column["name"]))
                continue
            slot = len(projections)
            if alias:
                alias_map[alias.lower()] = slot
            if expr[0] == "column":
                alias_map.setdefault(expr[1].lower(), slot)
            columns.append(alias or self._label(expr))
            projections.append(expr)

        rows = []
        for context in source:
            rows.append([self._eval(expr, context) for expr in projections])
        if distinct:
            deduped = []
            seen = set()
            for row in rows:
                marker = json.dumps([json_safe(value) for value in row], ensure_ascii=False, sort_keys=True)
                if marker in seen:
                    continue
                seen.add(marker)
                deduped.append(row)
            rows = deduped
        if order:
            rows = self._order_rows(rows, alias_map, order, source)
        total = len(rows)
        if offset:
            rows = rows[offset:]
        if limit is not None:
            rows = rows[:limit]
        _ = total
        return {"columns": columns, "rows": [[json_safe(value) for value in row] for row in rows]}

    def _order_rows(self, rows, alias_map, order, source):
        pairs = list(zip(rows, source))
        # Primary key ascending is the tie-breaker: sort by it first, then let the
        # stable sorts below refine from the last ORDER BY term to the first.
        pairs.sort(key=lambda pair: self._sort_key(pair[1]["pk"]))
        for expr, descending in reversed(order):
            if expr[0] == "column":
                slot = alias_map.get(expr[1].lower())
                if slot is not None:
                    pairs.sort(key=lambda pair: self._sort_key(pair[0][slot]), reverse=descending)
                    continue
            pairs.sort(
                key=lambda pair: self._sort_key(self._eval(expr, pair[1])),
                reverse=descending,
            )
        return [pair[0] for pair in pairs]

    @staticmethod
    def _sort_key(value):
        if value is None:
            return (0, 0.0, "")
        if isinstance(value, bool):
            return (1, float(value), "")
        if isinstance(value, (int, float)):
            return (1, float(value), "")
        if isinstance(value, (list, tuple)):
            return (2, float(len(value)), "")
        return (3, 0.0, str(value))

    def _select_aggregate(self, items, source, group, order, limit, offset):
        buckets = {}
        if group:
            for context in source:
                key = tuple(
                    self._sort_key(self._eval(expr, context)) for expr in group
                )
                buckets.setdefault(key, []).append(context)
        else:
            buckets[("__all__",)] = list(source)
        columns = []
        for expr, alias in items:
            columns.append(alias or self._label(expr))
        rows = []
        for contexts in buckets.values():
            rows.append([
                self._eval_aggregate(expr, contexts) for expr, _ in items
            ])
        if order:
            rows.sort(key=lambda row: self._sort_key(row[0]))
        if offset:
            rows = rows[offset:]
        if limit is not None:
            rows = rows[:limit]
        return {"columns": columns, "rows": [[json_safe(value) for value in row] for row in rows]}

    def _eval_aggregate(self, expr, contexts):
        if _is_aggregate(expr):
            _, name, arguments = expr
            if name == "COUNT":
                if arguments and arguments[0] == ("*", None):
                    return len(contexts)
                total = 0
                for context in contexts:
                    if self._eval(arguments[0], context) is not None:
                        total += 1
                return total
            values = [
                self._eval(arguments[0], context)
                for context in contexts
                if self._eval(arguments[0], context) is not None
            ]
            if not values:
                return None
            if name == "SUM":
                return sum(values)
            if name == "AVG":
                return sum(values) / len(values)
            if name == "MIN":
                return min(values, key=self._sort_key)
            if name == "MAX":
                return max(values, key=self._sort_key)
        if not contexts:
            return None
        return self._eval(expr, contexts[0])

    @staticmethod
    def _label(expr) -> str:
        if expr[0] == "column":
            return expr[1]
        if expr[0] == "call":
            name, arguments = expr[1], expr[2]
            if name == "COUNT" and arguments and arguments[0] == ("*", None):
                return f"{name}(*)"
            rendered = ", ".join(Engine._label(argument) for argument in arguments)
            return f"{name}({rendered})"
        if expr[0] == "literal":
            return "literal"
        return "expr"

    # -- expression evaluation -------------------------------------------- #
    @staticmethod
    def _truthy(value) -> bool:
        if value is None:
            return False
        if isinstance(value, bool):
            return value
        if isinstance(value, (int, float)):
            return value != 0
        if isinstance(value, str):
            return value != ""
        return True

    def _eval(self, node, context):
        kind = node[0]
        if kind == "literal":
            return node[1]
        if kind == "column":
            row = context.get("row")
            table = context.get("table")
            if row is None or table is None:
                raise fail("unknown_column", f"column {node[1]} needs a table")
            index = self._column_index(table, node[1])
            return row[index]
        if kind == "and":
            return self._truthy(self._eval(node[1], context)) and self._truthy(self._eval(node[2], context))
        if kind == "or":
            return self._truthy(self._eval(node[1], context)) or self._truthy(self._eval(node[2], context))
        if kind == "not":
            return not self._truthy(self._eval(node[1], context))
        if kind == "compare":
            return self._compare(node[1], self._eval(node[2], context), self._eval(node[3], context))
        if kind == "isnull":
            value = self._eval(node[1], context)
            return (value is not None) if node[2] else (value is None)
        if kind == "in":
            value = self._eval(node[1], context)
            return any(self._equals(value, self._eval(item, context)) for item in node[2])
        if kind == "like":
            value = self._eval(node[1], context)
            pattern = self._eval(node[2], context)
            return self._like(value, pattern)
        if kind == "arith":
            return self._arith(node[1], self._eval(node[2], context), self._eval(node[3], context))
        if kind == "call":
            return self._call(node[1], node[2], context)
        raise fail("unsupported_sql", f"unsupported expression {kind}")

    def _call(self, name, arguments, context):
        if name in AGGREGATES:
            raise fail("syntax_error", f"{name} cannot be used here")
        if name == "L2_DISTANCE":
            return self._distance(arguments, context)
        if name in ("COSINE_DISTANCE", "INNER_PRODUCT", "L2_SQUARED"):
            return self._vector_metric(name, arguments, context)
        if name == "ABS":
            return abs(self._eval(arguments[0], context))
        if name in ("LENGTH", "CHAR_LENGTH"):
            return len(self._eval(arguments[0], context))
        if name == "UPPER":
            return str(self._eval(arguments[0], context)).upper()
        if name == "LOWER":
            return str(self._eval(arguments[0], context)).lower()
        raise fail("unsupported_sql", f"function {name} is not supported")

    def _distance(self, arguments, context):
        left = self._eval(arguments[0], context)
        right = self._eval(arguments[1], context)
        vector_a = self._as_vector(left)
        vector_b = self._as_vector(right, len(vector_a))
        if len(vector_a) != len(vector_b):
            raise fail("dimension_mismatch", "vector dimensions differ")
        total = 0.0
        for item_a, item_b in zip(vector_a, vector_b):
            delta = item_a - item_b
            total += delta * delta
        return math.sqrt(total)

    def _vector_metric(self, name, arguments, context):
        vector_a = self._as_vector(self._eval(arguments[0], context))
        vector_b = self._as_vector(self._eval(arguments[1], context), len(vector_a))
        if len(vector_a) != len(vector_b):
            raise fail("dimension_mismatch", "vector dimensions differ")
        if name == "L2_SQUARED":
            total = 0.0
            for item_a, item_b in zip(vector_a, vector_b):
                delta = item_a - item_b
                total += delta * delta
            return total
        if name == "INNER_PRODUCT":
            return sum(item_a * item_b for item_a, item_b in zip(vector_a, vector_b))
        dot = sum(item_a * item_b for item_a, item_b in zip(vector_a, vector_b))
        norm_a = math.sqrt(sum(item * item for item in vector_a))
        norm_b = math.sqrt(sum(item * item for item in vector_b))
        if norm_a == 0 or norm_b == 0:
            return 0.0
        return dot / (norm_a * norm_b)

    def _as_vector(self, value, dim=None):
        if isinstance(value, (list, tuple)):
            return parse_vector_text(value, dim)
        if isinstance(value, str):
            return parse_vector_text(value, dim)
        raise fail("invalid_vector", "vector value expected")

    @staticmethod
    def _equals(left, right) -> bool:
        if left is None or right is None:
            return False
        if isinstance(left, bool) or isinstance(right, bool):
            return left is right
        return left == right

    def _compare(self, operator, left, right):
        if left is None or right is None:
            return False
        if operator == "=":
            return self._equals(left, right)
        if operator == "!=":
            return not self._equals(left, right)
        try:
            if operator == "<":
                return left < right
            if operator == "<=":
                return left <= right
            if operator == ">":
                return left > right
            if operator == ">=":
                return left >= right
        except TypeError:
            raise fail("type_mismatch", "cannot compare these values") from None
        raise fail("unsupported_sql", f"operator {operator} is not supported")

    @staticmethod
    def _like(value, pattern):
        if value is None or pattern is None:
            return False
        text = str(value)
        source = []
        index = 0
        while index < len(pattern):
            char = pattern[index]
            if char == "\\" and index + 1 < len(pattern):
                source.append("\\" + pattern[index + 1])
                index += 2
                continue
            if char == "%":
                source.append(".*")
            elif char == "_":
                source.append(".")
            else:
                source.append(char.replace("\\", "\\\\").replace(".", "\\."))
                source[-1] = (
                    source[-1]
                    .replace("(", "\\(")
                    .replace(")", "\\)")
                    .replace("[", "\\[")
                    .replace("]", "\\]")
                    .replace("{", "\\{")
                    .replace("}", "\\}")
                    .replace("+", "\\+")
                    .replace("*", "\\*")
                    .replace("?", "\\?")
                    .replace("^", "\\^")
                    .replace("$", "\\$")
                )
            index += 1
        import re

        return re.fullmatch("".join(source), text, re.DOTALL) is not None

    def _arith(self, operator, left, right):
        if left is None or right is None:
            return None
        if operator == "+":
            return left + right
        if operator == "-":
            return left - right
        if operator == "*":
            return left * right
        if operator == "/":
            if right == 0:
                raise fail("division_by_zero", "division by zero")
            return left / right
        raise fail("unsupported_sql", f"operator {operator} is not supported")


# --------------------------------------------------------------------------- #
# protocol loop
# --------------------------------------------------------------------------- #
def data_directory() -> Path:
    override = os.environ.get("VECTORDB_DATA_DIR")
    if override:
        return Path(override)
    return Path("/data")


def emit(stream, message) -> None:
    stream.write(json.dumps(message, ensure_ascii=False, allow_nan=False).encode("utf-8") + b"\n")
    stream.flush()


def main() -> int:
    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    engine = Engine(data_directory())
    emit(stdout, {"type": "ready"})
    try:
        for raw in stdin:
            line = raw.strip()
            if not line:
                continue
            try:
                request = json.loads(line.decode("utf-8"))
            except (ValueError, UnicodeError) as error:
                emit(stdout, {
                    "id": None,
                    "ok": False,
                    "error": {"code": "invalid_request", "message": f"invalid JSON request: {error}"},
                })
                continue
            if not isinstance(request, dict):
                emit(stdout, {
                    "id": None,
                    "ok": False,
                    "error": {"code": "invalid_request", "message": "request must be an object"},
                })
                continue
            identifier = request.get("id")
            operation = request.get("op")
            if operation == "shutdown":
                try:
                    engine.close()
                except Exception:  # pragma: no cover - best effort durability
                    pass
                emit(stdout, {"id": identifier, "ok": True})
                return 0
            if operation != "execute":
                emit(stdout, {
                    "id": identifier,
                    "ok": False,
                    "error": {"code": "unsupported_op", "message": f"unsupported op {operation!r}"},
                })
                continue
            sql = request.get("sql")
            if not isinstance(sql, str):
                emit(stdout, {
                    "id": identifier,
                    "ok": False,
                    "error": {"code": "invalid_request", "message": "sql must be a string"},
                })
                continue
            mode = request.get("search_mode") or "exact"
            try:
                result = engine.execute(sql, mode)
            except SQLError as error:
                emit(stdout, {
                    "id": identifier,
                    "ok": False,
                    "error": {"code": error.code, "message": error.message},
                })
                continue
            except RecursionError:
                emit(stdout, {
                    "id": identifier,
                    "ok": False,
                    "error": {"code": "unsupported_sql", "message": "statement is too deeply nested"},
                })
                continue
            except Exception as error:  # pragma: no cover - defensive
                emit(stdout, {
                    "id": identifier,
                    "ok": False,
                    "error": {"code": "internal_error", "message": str(error)},
                })
                continue
            message = {"id": identifier, "ok": True}
            message.update(result)
            try:
                emit(stdout, message)
            except (ValueError, TypeError) as error:
                emit(stdout, {
                    "id": identifier,
                    "ok": False,
                    "error": {"code": "internal_error", "message": str(error)},
                })
    finally:
        try:
            engine.close()
        except Exception:  # pragma: no cover - closed twice on shutdown path
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
