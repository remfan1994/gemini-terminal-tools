#!/usr/bin/env python3
"""
ttychatter.mailbot
==================

Mail transport adapter for ttychatter.

This program intentionally does *not* contain OpenRouter request logic.  The
existing ttychatter clients already know how to talk to the model backend, keep
sessions, rebuild context, save attachments, and format response logs.  The
mailbot is therefore only a transport bridge:

    IMAP message -> ttychatter subprocess -> SMTP/sendmail reply

That design keeps the project maintainable.  If the OpenRouter code changes,
or if ttychatter later supports more providers, this mailbot should keep working
as long as the command-line ttychatter client keeps a compatible loopback mode.

The first implementation uses only the Python standard library plus optional
external `gpg`.  That keeps it suitable for OpenBSD/Dovecot/smtpd-style setups
without bringing in a web framework, daemon framework, or mail library from pip.
"""

from __future__ import annotations

import argparse
from collections import deque
import email
import email.policy
import getpass
import gzip
import html
import imaplib
import mimetypes
import os
import re
import shlex
import smtplib
import ssl
import subprocess
import sys
import tempfile
import time
import textwrap
from dataclasses import dataclass, field
from email.message import EmailMessage, Message
from email.utils import getaddresses, make_msgid, parseaddr
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

PROGRAM = "ttychatter.mailbot"
VERSION = "0.3.0"

DEFAULT_CONFIG_DIR = Path.home() / ".config" / "ttychatter" / "mailbot"
DEFAULT_CONFIG_FILE = DEFAULT_CONFIG_DIR / "config"
DEFAULT_STATE_DIR = Path.home() / ".local" / "state" / "ttychatter" / "mailbot"
DEFAULT_CACHE_DIR = Path.home() / ".cache" / "ttychatter" / "mailbot"
DEFAULT_SESSION_DIR = Path.home() / ".local" / "share" / "ttychatter" / "openrouter" / "sessions"
DEFAULT_ATTACHMENT_DIR = Path.home() / ".local" / "share" / "ttychatter" / "openrouter" / "attachments"
DEFAULT_MODEL_CACHE_FILE = Path.home() / ".config" / "ttychatter" / "openrouter" / "models-cache.json"


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
# The config parser is deliberately boring: KEY=VALUE lines, blank lines, and
# comments.  This is the same spirit as the rest of ttychatter.  A future
# maintainer should resist the urge to require YAML/TOML/JSON just to start a
# mail transport daemon.  Mailbots often run in plain server environments where
# minimalism and readability are more valuable than fancy config syntax.
# ---------------------------------------------------------------------------


@dataclass
class Config:
    config_file: Path = DEFAULT_CONFIG_FILE

    imap_host: str = "localhost"
    imap_port: int = 993
    imap_ssl: bool = True
    imap_user: str = ""
    imap_password: str = ""
    imap_password_gpg_file: Path = DEFAULT_CONFIG_DIR / "imap-password.gpg"
    imap_mailbox: str = "INBOX"

    send_method: str = "sendmail"  # sendmail or smtp
    config_subject_token: str = "CONFIG"
    mailbot_subject_token: str = "MAILBOT"
    mailbot_manpage_file: Optional[Path] = None
    config_update_requires_attachment: bool = True
    sendmail_path: str = "/usr/sbin/sendmail"

    smtp_host: str = "localhost"
    smtp_port: int = 25
    smtp_ssl: bool = False
    smtp_starttls: bool = False
    smtp_user: str = ""
    smtp_password: str = ""
    smtp_password_gpg_file: Path = DEFAULT_CONFIG_DIR / "smtp-password.gpg"

    mail_from: str = ""
    mail_to: str = ""
    allowed_from: List[str] = field(default_factory=list)

    subject_prefix: str = "tc"
    poll_interval: int = 60
    max_messages_per_run: int = 10
    max_body_bytes: int = 50000
    max_attachment_bytes: int = 1048576
    process_attachments: bool = True
    mark_seen: bool = True

    ttychatter_command: str = "ttychatter.python"
    ttychatter_extra_args: str = ""
    ttychatter_control_timeout: int = 180
    session_dir: Path = DEFAULT_SESSION_DIR
    attachment_dir: Path = DEFAULT_ATTACHMENT_DIR
    model_cache_file: Path = DEFAULT_MODEL_CACHE_FILE
    work_dir: Path = DEFAULT_CACHE_DIR / "work"
    processed_db: Path = DEFAULT_STATE_DIR / "processed-ids.txt"

    session_list_head_lines: int = 3
    session_list_tail_lines: int = 3
    session_list_max_sessions: int = 50
    session_preview_line_chars: int = 220
    session_search_max_matches: int = 100
    max_command_output_bytes: int = 200000
    max_outgoing_attachment_bytes: int = 8388608

    reply_subject_prefix: str = "Re:"
    include_ttychatter_stderr: bool = False


@dataclass
class ReplyAttachment:
    filename: str
    data: bytes
    maintype: str = "text"
    subtype: str = "plain"


def expand_path(value: str) -> Path:
    return Path(os.path.expandvars(os.path.expanduser(value))).resolve()


def parse_bool(value: str, default: bool = False) -> bool:
    if value == "":
        return default
    return value.strip().lower() in {"1", "yes", "y", "true", "on"}


def parse_int(value: str, default: int) -> int:
    try:
        return int(value)
    except ValueError:
        return default


def split_addresses(value: str) -> List[str]:
    """Normalize a user supplied address list.

    ALLOWED_FROM is security-sensitive.  A human may write it as one address,
    a comma-separated list, a semicolon-separated list, or a whitespace-separated
    list.  The mail headers are later parsed with email.utils.parseaddr(), so
    this function stores only lowercase bare addresses.  It deliberately accepts
    the wildcard "*" for temporary lab testing, but administrators should not
    leave that enabled on a real public mailbox.
    """
    out: List[str] = []
    for item in re.split(r"[;,\s]+", value.strip()):
        item = item.strip()
        if not item:
            continue
        if item == "*":
            out.append("*")
            continue
        parsed = parseaddr(item)[1].strip().lower()
        if parsed:
            out.append(parsed)
    return out


def set_config_value(config_file: Path, key: str, value: str) -> None:
    config_file.parent.mkdir(parents=True, exist_ok=True)
    lines: List[str] = []
    replaced = False
    if config_file.exists():
        lines = config_file.read_text(encoding="utf-8", errors="replace").splitlines()
    out: List[str] = []
    for line in lines:
        if line.strip().startswith(f"{key}="):
            out.append(f"{key}={value}")
            replaced = True
        else:
            out.append(line)
    if not replaced:
        out.append(f"{key}={value}")
    config_file.write_text("\n".join(out) + "\n", encoding="utf-8")
    try:
        os.chmod(config_file, 0o600)
    except OSError:
        pass


def unset_config_value(config_file: Path, key: str) -> None:
    if not config_file.exists():
        return
    lines = config_file.read_text(encoding="utf-8", errors="replace").splitlines()
    out = [line for line in lines if not line.strip().startswith(f"{key}=")]
    config_file.write_text("\n".join(out) + ("\n" if out else ""), encoding="utf-8")


def load_config(path: Path = DEFAULT_CONFIG_FILE) -> Config:
    cfg = Config(config_file=path)
    if not path.exists():
        return cfg

    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()

        if key == "IMAP_HOST": cfg.imap_host = value
        elif key == "IMAP_PORT": cfg.imap_port = parse_int(value, cfg.imap_port)
        elif key == "IMAP_SSL": cfg.imap_ssl = parse_bool(value, cfg.imap_ssl)
        elif key == "IMAP_USER": cfg.imap_user = value
        elif key == "IMAP_PASSWORD": cfg.imap_password = value
        elif key == "IMAP_PASSWORD_GPG_FILE": cfg.imap_password_gpg_file = expand_path(value)
        elif key == "IMAP_MAILBOX": cfg.imap_mailbox = value

        elif key == "SEND_METHOD": cfg.send_method = value.lower()
        elif key == "REPLY_METHOD": cfg.send_method = value.lower()
        elif key == "SENDMAIL": cfg.sendmail_path = value
        elif key == "SENDMAIL_PATH": cfg.sendmail_path = value

        elif key == "SMTP_HOST": cfg.smtp_host = value
        elif key == "SMTP_PORT": cfg.smtp_port = parse_int(value, cfg.smtp_port)
        elif key == "SMTP_SSL": cfg.smtp_ssl = parse_bool(value, cfg.smtp_ssl)
        elif key == "SMTP_STARTTLS": cfg.smtp_starttls = parse_bool(value, cfg.smtp_starttls)
        elif key == "SMTP_USER": cfg.smtp_user = value
        elif key == "SMTP_PASSWORD": cfg.smtp_password = value
        elif key == "SMTP_PASSWORD_GPG_FILE": cfg.smtp_password_gpg_file = expand_path(value)

        elif key == "MAIL_FROM": cfg.mail_from = value
        elif key == "MAIL_TO": cfg.mail_to = value
        elif key == "ALLOWED_FROM": cfg.allowed_from = split_addresses(value)

        elif key == "SUBJECT_PREFIX": cfg.subject_prefix = value
        elif key == "POLL_INTERVAL": cfg.poll_interval = max(1, parse_int(value, cfg.poll_interval))
        elif key == "MAX_MESSAGES_PER_RUN": cfg.max_messages_per_run = max(1, parse_int(value, cfg.max_messages_per_run))
        elif key == "MAX_BODY_BYTES": cfg.max_body_bytes = max(1, parse_int(value, cfg.max_body_bytes))
        elif key == "MAX_ATTACHMENT_BYTES": cfg.max_attachment_bytes = max(1, parse_int(value, cfg.max_attachment_bytes))
        elif key == "PROCESS_ATTACHMENTS": cfg.process_attachments = parse_bool(value, cfg.process_attachments)
        elif key == "MARK_SEEN": cfg.mark_seen = parse_bool(value, cfg.mark_seen)

        elif key == "TTYCHATTER_COMMAND": cfg.ttychatter_command = value
        elif key == "TTYCHATTER_EXTRA_ARGS": cfg.ttychatter_extra_args = value
        elif key == "SESSION_DIR": cfg.session_dir = expand_path(value)
        elif key == "ATTACHMENT_DIR": cfg.attachment_dir = expand_path(value)
        elif key == "WORK_DIR": cfg.work_dir = expand_path(value)
        elif key == "PROCESSED_DB": cfg.processed_db = expand_path(value)

        elif key == "REPLY_SUBJECT_PREFIX": cfg.reply_subject_prefix = value
        elif key == "INCLUDE_TTYCHATTER_STDERR": cfg.include_ttychatter_stderr = parse_bool(value, cfg.include_ttychatter_stderr)
        elif key == "CONFIG_SUBJECT_TOKEN": cfg.config_subject_token = value
        elif key == "MAILBOT_SUBJECT_TOKEN": cfg.mailbot_subject_token = value
        elif key == "MAILBOT_MANPAGE_FILE": cfg.mailbot_manpage_file = expand_path(value) if value else None
        elif key == "CONFIG_UPDATE_REQUIRES_ATTACHMENT": cfg.config_update_requires_attachment = parse_bool(value, cfg.config_update_requires_attachment)
        elif key == "MODEL_CACHE_FILE": cfg.model_cache_file = expand_path(value)
        elif key == "TTYCHATTER_CONTROL_TIMEOUT": cfg.ttychatter_control_timeout = max(1, parse_int(value, cfg.ttychatter_control_timeout))
        elif key == "SESSION_LIST_HEAD_LINES": cfg.session_list_head_lines = max(0, parse_int(value, cfg.session_list_head_lines))
        elif key == "SESSION_LIST_TAIL_LINES": cfg.session_list_tail_lines = max(0, parse_int(value, cfg.session_list_tail_lines))
        elif key == "SESSION_LIST_MAX_SESSIONS": cfg.session_list_max_sessions = max(1, parse_int(value, cfg.session_list_max_sessions))
        elif key == "SESSION_PREVIEW_LINE_CHARS": cfg.session_preview_line_chars = max(20, parse_int(value, cfg.session_preview_line_chars))
        elif key == "SESSION_SEARCH_MAX_MATCHES": cfg.session_search_max_matches = max(1, parse_int(value, cfg.session_search_max_matches))
        elif key == "MAX_COMMAND_OUTPUT_BYTES": cfg.max_command_output_bytes = max(1000, parse_int(value, cfg.max_command_output_bytes))
        elif key == "MAX_OUTGOING_ATTACHMENT_BYTES": cfg.max_outgoing_attachment_bytes = max(1, parse_int(value, cfg.max_outgoing_attachment_bytes))

    # Environment overrides are useful on servers, cron jobs, and test runs.
    cfg.imap_password = os.environ.get("TTYCHATTER_IMAP_PASSWORD", cfg.imap_password)
    cfg.smtp_password = os.environ.get("TTYCHATTER_SMTP_PASSWORD", cfg.smtp_password)
    env_manpage = os.environ.get("TTYCHATTER_MAILBOT_MANPAGE_FILE", "").strip()
    if env_manpage:
        cfg.mailbot_manpage_file = expand_path(env_manpage)
    return cfg


# ---------------------------------------------------------------------------
# GPG helpers
# ---------------------------------------------------------------------------
# The mailbot supports encrypted IMAP/SMTP passwords using external gpg.  This
# mirrors ttychatter's API-key philosophy: do not implement crypto in the app;
# call a standard Unix tool that users already understand and can replace.
# ---------------------------------------------------------------------------


def gpg_available() -> bool:
    from shutil import which
    return which("gpg") is not None


def decrypt_gpg_file(path: Path) -> str:
    proc = subprocess.run(
        ["gpg", "--quiet", "--decrypt", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"gpg failed decrypting {path}")
    return proc.stdout.strip("\n")


def encrypt_secret_to_gpg(path: Path, secret: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(
        ["gpg", "--symmetric", "--cipher-algo", "AES256", "--yes", "--output", str(path)],
        input=secret,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"gpg failed encrypting {path}")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass


def load_password(plain: str, gpg_path: Path) -> str:
    if plain:
        return plain
    if gpg_path.exists():
        return decrypt_gpg_file(gpg_path)
    return ""


# ---------------------------------------------------------------------------
# Message parsing
# ---------------------------------------------------------------------------
# Email is messy.  The mailbot keeps a deliberately conservative parser: prefer
# text/plain, ignore untrusted HTML when possible, size-limit message bodies, and
# save attachments into an isolated work directory before handing them to
# ttychatter.  Future maintainers should be careful here.  A mailbot is a bridge
# from the public mail world to a paid AI API; every boundary needs limits.
# ---------------------------------------------------------------------------


def subject_regex(prefix: str) -> re.Pattern[str]:
    return re.compile(rf"\b{re.escape(prefix)}:\{{([^}}]+)\}}")


def parse_session_from_subject(subject: str, prefix: str) -> Optional[str]:
    match = subject_regex(prefix).search(subject or "")
    if not match:
        return None
    session = match.group(1).strip()
    if not session:
        return None
    if session.lower() == "new":
        return "session-" + time.strftime("%Y-%m-%d-%H-%M-%S")
    return session


def is_config_request(session: str, cfg: Config) -> bool:
    """Return true when a tc:{...} token names the protected config workflow.

    The mailbot intentionally treats tc:{CONFIG} as a transport-control command,
    not as an AI session name.  This keeps configuration editing through mail
    explicit, visible, and auditable.  The command still must pass the normal
    ALLOWED_FROM check before it can do anything.
    """
    return session.strip().lower() == cfg.config_subject_token.strip().lower()


def decode_part_payload(part: Message) -> str:
    payload = part.get_payload(decode=True)
    if payload is None:
        raw = part.get_payload()
        return raw if isinstance(raw, str) else ""
    charset = part.get_content_charset() or "utf-8"
    return payload.decode(charset, errors="replace")


def strip_html(text: str) -> str:
    text = re.sub(r"(?is)<(script|style).*?>.*?</\1>", "", text)
    text = re.sub(r"(?s)<br\s*/?>", "\n", text)
    text = re.sub(r"(?s)</p>", "\n\n", text)
    text = re.sub(r"(?s)<.*?>", "", text)
    return html.unescape(text).strip()


def extract_body(msg: Message, max_body_bytes: int) -> str:
    text_plain: List[str] = []
    text_html: List[str] = []

    if msg.is_multipart():
        for part in msg.walk():
            if part.is_multipart():
                continue
            disposition = (part.get_content_disposition() or "").lower()
            if disposition == "attachment":
                continue
            content_type = part.get_content_type()
            if content_type == "text/plain":
                text_plain.append(decode_part_payload(part))
            elif content_type == "text/html":
                text_html.append(strip_html(decode_part_payload(part)))
    else:
        if msg.get_content_type() == "text/html":
            text_html.append(strip_html(decode_part_payload(msg)))
        else:
            text_plain.append(decode_part_payload(msg))

    body = "\n".join(x for x in text_plain if x.strip()).strip()
    if not body:
        body = "\n".join(x for x in text_html if x.strip()).strip()

    raw = body.encode("utf-8", errors="replace")
    if len(raw) > max_body_bytes:
        raw = raw[:max_body_bytes]
        body = raw.decode("utf-8", errors="replace") + "\n\n[message body truncated by ttychatter.mailbot]"
    return body.strip()


def safe_filename(name: str) -> str:
    name = os.path.basename(name or "attachment")
    name = re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("._-")
    return name or "attachment"


def save_attachments(msg: Message, directory: Path, max_attachment_bytes: int) -> List[Path]:
    directory.mkdir(parents=True, exist_ok=True)
    saved: List[Path] = []
    counter = 1
    for part in msg.walk():
        if part.is_multipart():
            continue
        disposition = (part.get_content_disposition() or "").lower()
        filename = part.get_filename()
        if disposition != "attachment" and not filename:
            continue
        payload = part.get_payload(decode=True)
        if payload is None:
            continue
        if len(payload) > max_attachment_bytes:
            continue
        base = safe_filename(filename or f"attachment-{counter}")
        path = directory / f"{counter:02d}-{base}"
        path.write_bytes(payload)
        saved.append(path)
        counter += 1
    return saved



def looks_like_config_text(text: str) -> bool:
    """Very small sanity check for emailed config files.

    This does not try to validate every possible key.  Its job is to prevent an
    obviously wrong attachment, such as an image or forwarded HTML body, from
    replacing the mailbot config.  Real parsing still happens through load_config
    after the file is written.
    """
    useful = 0
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if '=' not in line:
            return False
        key, _ = line.split('=', 1)
        if not re.match(r'^[A-Z0-9_]+$', key.strip()):
            return False
        useful += 1
    return useful > 0


def find_config_attachment(msg: Message, max_attachment_bytes: int) -> Optional[Tuple[str, str]]:
    """Return (filename, text) for the first plausible config attachment.

    The config-over-email workflow is intentionally attachment-based.  The bot
    sends the current config as an attached text file, the user edits that file,
    and then mails it back with subject tc:{CONFIG}.  Requiring an attachment
    avoids accidentally treating a normal email body as a replacement config.
    """
    for part in msg.walk():
        if part.is_multipart():
            continue
        filename = part.get_filename() or ''
        disposition = (part.get_content_disposition() or '').lower()
        if disposition != 'attachment' and not filename:
            continue
        safe = safe_filename(filename or 'config.txt')
        if not (safe.endswith('.txt') or 'config' in safe.lower()):
            continue
        payload = part.get_payload(decode=True)
        if payload is None or len(payload) > max_attachment_bytes:
            continue
        charset = part.get_content_charset() or 'utf-8'
        text = payload.decode(charset, errors='replace')
        if looks_like_config_text(text):
            return safe, text
    return None


def build_config_reply(cfg: Config, original: Message, body: str, attachment_text: Optional[str] = None) -> EmailMessage:
    """Build a reply for tc:{CONFIG}.

    If attachment_text is provided, it is attached as a text/plain config file.
    This is the safe round-trip path: request config, receive attachment, edit
    locally, reply with edited attachment.
    """
    reply_to = parseaddr(original.get('Reply-To') or original.get('From') or '')[1]
    dest = cfg.mail_to or reply_to
    subject = original.get('Subject', f'{cfg.subject_prefix}:{{{cfg.config_subject_token}}}')
    msg = EmailMessage()
    msg['From'] = cfg.mail_from or cfg.smtp_user or cfg.imap_user
    msg['To'] = dest
    msg['Subject'] = f'{cfg.reply_subject_prefix} {subject}'
    msg['Message-ID'] = make_msgid(domain=(msg['From'].split('@')[-1] if '@' in msg['From'] else None))
    if original.get('Message-ID'):
        msg['In-Reply-To'] = original.get('Message-ID')
        msg['References'] = original.get('Message-ID')
    msg.set_content(body.rstrip() + '\n')
    if attachment_text is not None:
        msg.add_attachment(attachment_text.encode('utf-8'), maintype='text', subtype='plain', filename='ttychatter-mailbot-config.txt')
    return msg


def public_config_text(cfg: Config) -> str:
    """Return the current config file, redacting plaintext password lines.

    The goal is editable configuration without accidentally emailing secrets in
    the clear.  Encrypted password file paths are safe to show; plaintext
    password values are replaced with a comment.
    """
    if not cfg.config_file.exists():
        return '# ttychatter.mailbot config does not exist yet\n'
    out=[]
    for raw in cfg.config_file.read_text(encoding='utf-8', errors='replace').splitlines():
        if raw.strip().startswith(('IMAP_PASSWORD=', 'SMTP_PASSWORD=')):
            key=raw.split('=',1)[0]
            out.append(f'# {key}=REDACTED_BY_TTYCHATTER_MAILBOT')
        else:
            out.append(raw)
    return '\n'.join(out).rstrip() + '\n'


def apply_config_attachment(cfg: Config, text: str) -> Path:
    """Atomically replace the config file with emailed config text.

    A timestamped backup is kept beside the original.  This is intentionally
    simple and inspectable: future maintainers can see exactly what file was
    replaced and where the backup went.
    """
    cfg.config_file.parent.mkdir(parents=True, exist_ok=True)
    backup = cfg.config_file.with_name(cfg.config_file.name + '.bak-' + time.strftime('%Y%m%d-%H%M%S'))
    if cfg.config_file.exists():
        backup.write_text(cfg.config_file.read_text(encoding='utf-8', errors='replace'), encoding='utf-8')
    tmp = cfg.config_file.with_suffix(cfg.config_file.suffix + '.tmp')
    tmp.write_text(text.rstrip() + '\n', encoding='utf-8')
    tmp.replace(cfg.config_file)
    try:
        os.chmod(cfg.config_file, 0o600)
    except OSError:
        pass
    return backup

# ---------------------------------------------------------------------------
# IMAP / SMTP transport
# ---------------------------------------------------------------------------
# IMAP is only used for receiving and state flags.  SMTP or sendmail is used for
# replies.  OpenBSD users with smtpd can set SEND_METHOD=sendmail and avoid SMTP
# password storage entirely.  Users with hosted mail can configure SMTP.
# ---------------------------------------------------------------------------


def imap_connect(cfg: Config) -> imaplib.IMAP4:
    password = load_password(cfg.imap_password, cfg.imap_password_gpg_file)
    if cfg.imap_ssl:
        conn: imaplib.IMAP4 = imaplib.IMAP4_SSL(cfg.imap_host, cfg.imap_port)
    else:
        conn = imaplib.IMAP4(cfg.imap_host, cfg.imap_port)
    conn.login(cfg.imap_user, password)
    conn.select(cfg.imap_mailbox)
    return conn


def message_sender(msg: Message) -> str:
    return parseaddr(msg.get("From", ""))[1].lower()


def allowed_sender(sender: str, cfg: Config) -> bool:
    """Return true when an incoming sender is authorized to drive the bot.

    The allowlist is the primary protection against random Internet mail
    spending API credits.  Earlier mailbot builds also rejected any message whose
    sender matched MAIL_FROM, intending to prevent reply loops.  That was too
    blunt: small personal mail systems often use the same mailbox for both
    sending and receiving during setup, and an explicit ALLOWED_FROM entry should
    be respected.

    Loop prevention belongs elsewhere: exact tc:{...} subjects, processed
    Message-IDs, dry-run safety, and optional deployment advice to use a distinct
    MAIL_FROM such as ttychatter@example.org.  Therefore this function now does
    only one job: compare the parsed sender against the configured allowlist.
    """
    parsed_sender = parseaddr(sender or "")[1].strip().lower()
    if not parsed_sender:
        return False
    allowed = {item.strip().lower() for item in cfg.allowed_from if item.strip()}
    if "*" in allowed:
        return True
    return parsed_sender in allowed


def load_processed(path: Path) -> set[str]:
    if not path.exists():
        return set()
    return {line.strip() for line in path.read_text(encoding="utf-8", errors="replace").splitlines() if line.strip()}


def remember_processed(path: Path, message_id: str) -> None:
    if not message_id:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        fh.write(message_id + "\n")


def send_reply_sendmail(cfg: Config, reply: EmailMessage) -> None:
    proc = subprocess.run(
        [cfg.sendmail_path, "-t", "-oi"],
        input=reply.as_string(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "sendmail failed")


def send_reply_smtp(cfg: Config, reply: EmailMessage) -> None:
    password = load_password(cfg.smtp_password, cfg.smtp_password_gpg_file)
    context = ssl.create_default_context()
    if cfg.smtp_ssl:
        smtp: smtplib.SMTP = smtplib.SMTP_SSL(cfg.smtp_host, cfg.smtp_port, context=context)
    else:
        smtp = smtplib.SMTP(cfg.smtp_host, cfg.smtp_port)
    try:
        smtp.ehlo()
        if cfg.smtp_starttls:
            smtp.starttls(context=context)
            smtp.ehlo()
        if cfg.smtp_user:
            smtp.login(cfg.smtp_user, password)
        smtp.send_message(reply)
    finally:
        smtp.quit()


def send_reply(cfg: Config, reply: EmailMessage) -> None:
    if cfg.send_method == "smtp":
        send_reply_smtp(cfg, reply)
    else:
        send_reply_sendmail(cfg, reply)


# ---------------------------------------------------------------------------
# ttychatter subprocess bridge
# ---------------------------------------------------------------------------
# The command is invoked in loopback mode so stdout is the clean AI response.
# Interface/status chatter belongs on stderr and is retained only for optional
# debugging.  This is the same reason the Bash clients gained loopback mode: it
# makes ttychatter composable with other Unix transports.
# ---------------------------------------------------------------------------


def session_log_path(cfg: Config, session: str) -> Path:
    session = sanitize_session_name(session)
    if session.endswith(".log"):
        return cfg.session_dir / session
    return cfg.session_dir / f"{session}.log"


def sanitize_session_name(session: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", (session or "").strip()).strip("._-") or "session"


def session_files_newest_first(cfg: Config) -> List[Path]:
    if not cfg.session_dir.exists():
        return []
    paths = [p for p in cfg.session_dir.glob("*.log") if p.is_file()]

    def mtime(path: Path) -> float:
        try:
            return path.stat().st_mtime
        except OSError:
            return 0.0

    paths.sort(key=lambda p: (mtime(p), p.name.lower()), reverse=True)
    return paths


def trim_preview_line(line: str, max_chars: int) -> str:
    line = line.rstrip("\r\n")
    line = re.sub(r"[\t ]+", " ", line)
    if len(line) > max_chars:
        return line[: max(0, max_chars - 3)] + "..."
    return line


def read_session_head_tail(path: Path, head_lines: int, tail_lines: int, line_chars: int) -> Tuple[List[str], List[str], int]:
    head: List[Tuple[int, str]] = []
    tail = deque(maxlen=max(0, tail_lines))
    total = 0
    try:
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            for idx, raw in enumerate(fh):
                line = trim_preview_line(raw, line_chars)
                if idx < head_lines:
                    head.append((idx, line))
                if tail_lines > 0:
                    tail.append((idx, line))
                total = idx + 1
    except OSError as exc:
        return [f"[could not read: {exc}]"], [], 0
    head_out = [line for _, line in head]
    tail_out = [line for idx, line in tail if idx >= head_lines]
    return head_out, tail_out, total


def format_session_list_with_previews(cfg: Config) -> str:
    paths = session_files_newest_first(cfg)
    if not paths:
        return f"no session logs found in {cfg.session_dir}\n"

    shown = paths[: cfg.session_list_max_sessions]
    lines: List[str] = [
        f"sessions in {cfg.session_dir}",
        f"count: {len(paths)}; showing: {len(shown)}; order: newest first",
        f"preview: head={cfg.session_list_head_lines} tail={cfg.session_list_tail_lines}",
        "",
    ]
    for path in shown:
        try:
            st = path.stat()
            modified = time.strftime("%Y-%m-%d %H:%M:%S %z", time.localtime(st.st_mtime))
            size = st.st_size
        except OSError:
            modified = "unknown"
            size = 0
        head, tail, total = read_session_head_tail(
            path,
            cfg.session_list_head_lines,
            cfg.session_list_tail_lines,
            cfg.session_preview_line_chars,
        )
        lines.append(f"== {path.stem} ==")
        lines.append(f"modified: {modified}; bytes: {size}; lines: {total}")
        lines.append("head:")
        if head:
            lines.extend(f"  {line}" for line in head)
        else:
            lines.append("  [empty]")
        lines.append("tail:")
        if tail:
            lines.extend(f"  {line}" for line in tail)
        else:
            lines.append("  [empty or already shown in head]")
        lines.append("")
    if len(paths) > len(shown):
        lines.append(f"[{len(paths) - len(shown)} older sessions not shown; raise SESSION_LIST_MAX_SESSIONS to include more]")
    return "\n".join(lines).rstrip() + "\n"


def search_session_logs(cfg: Config, query: str, session: Optional[str] = None) -> str:
    query = (query or "").strip()
    if not query:
        return "ERROR: search requires text\n"
    if session:
        paths = [session_log_path(cfg, session)]
    else:
        paths = session_files_newest_first(cfg)
    if not paths:
        return f"no session logs found in {cfg.session_dir}\n"
    needle = query.casefold()
    found = 0
    lines: List[str] = [f"search: {query}", ""]
    for path in paths:
        if not path.exists():
            continue
        file_matches: List[str] = []
        try:
            with path.open("r", encoding="utf-8", errors="replace") as fh:
                for lineno, raw in enumerate(fh, 1):
                    if needle in raw.casefold():
                        file_matches.append(f"{lineno}: {trim_preview_line(raw, cfg.session_preview_line_chars)}")
                        found += 1
                        if found >= cfg.session_search_max_matches:
                            break
        except OSError as exc:
            file_matches.append(f"[could not read: {exc}]")
        if file_matches:
            lines.append(f"--- {path.name} ---")
            lines.extend(file_matches)
            lines.append("")
        if found >= cfg.session_search_max_matches:
            break
    if found == 0:
        lines.append(f"no matches for: {query}")
    elif found >= cfg.session_search_max_matches:
        lines.append(f"[stopped after SESSION_SEARCH_MAX_MATCHES={cfg.session_search_max_matches}]")
    return "\n".join(lines).rstrip() + "\n"


def rename_session_file(cfg: Config, old: str, new: str) -> str:
    old_name = sanitize_session_name(old)
    new_name = sanitize_session_name(new)
    old_path = session_log_path(cfg, old_name)
    new_path = session_log_path(cfg, new_name)
    if not old_path.exists():
        raise FileNotFoundError(f"session not found: {old_name}")
    if new_path.exists():
        raise FileExistsError(f"target session already exists: {new_name}")
    new_path.parent.mkdir(parents=True, exist_ok=True)
    old_path.rename(new_path)
    return f"renamed session: {old_name} -> {new_name}\n"


def run_ttychatter(cfg: Config, session: str, body: str, attachments: List[Path]) -> Tuple[int, str, str]:
    cfg.session_dir.mkdir(parents=True, exist_ok=True)
    cfg.attachment_dir.mkdir(parents=True, exist_ok=True)

    session_name = session_log_path(cfg, session).stem
    cmd = shlex.split(cfg.ttychatter_command)
    cmd.append("--loopback")
    for extra in shlex.split(cfg.ttychatter_extra_args):
        cmd.append(extra)
    for path in attachments:
        cmd.extend(["--attach", str(path)])
    cmd.append(session_name)

    proc = subprocess.run(
        cmd,
        input=body,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def run_ttychatter_cli(cfg: Config, args: List[str], input_text: str = "", timeout: Optional[int] = None) -> Tuple[int, str, str]:
    """Run a noninteractive ttychatter.python utility command."""
    cmd = shlex.split(cfg.ttychatter_command) + [str(arg) for arg in args]
    try:
        proc = subprocess.run(
            cmd,
            input=input_text,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout or cfg.ttychatter_control_timeout,
        )
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()
    except FileNotFoundError:
        return 127, "", f"command not found: {cmd[0] if cmd else cfg.ttychatter_command}"
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout if isinstance(exc.stdout, str) else ""
        stderr = exc.stderr if isinstance(exc.stderr, str) else ""
        stderr = (stderr + "\n" if stderr else "") + f"command timed out after {timeout or cfg.ttychatter_control_timeout} seconds"
        return 124, stdout.strip(), stderr.strip()


def truncate_text_bytes(text: str, max_bytes: int) -> str:
    raw = (text or "").encode("utf-8", errors="replace")
    if len(raw) <= max_bytes:
        return text or ""
    return raw[:max_bytes].decode("utf-8", errors="replace") + f"\n\n[truncated to {max_bytes} bytes by ttychatter.mailbot]"


def format_cli_result(label: str, rc: int, stdout: str, stderr_text: str, max_bytes: int) -> str:
    combined_budget = max(1000, max_bytes)
    lines = [label, f"exit_status: {rc}"]
    if stdout:
        lines.extend(["", "--- stdout ---", truncate_text_bytes(stdout, combined_budget)])
    if stderr_text:
        lines.extend(["", "--- stderr ---", truncate_text_bytes(stderr_text, combined_budget)])
    if not stdout and not stderr_text:
        lines.extend(["", "(no output)"])
    return "\n".join(lines).rstrip() + "\n"


def parse_updated_model_cache_path(output: str) -> Optional[Path]:
    for line in (output or "").splitlines():
        match = re.search(r"updated model cache:\s*(.+)$", line)
        if match:
            return expand_path(match.group(1).strip())
    return None


def read_file_attachment(path: Path, cfg: Config, filename: Optional[str] = None) -> Tuple[Optional[ReplyAttachment], str]:
    try:
        if not path.exists() or not path.is_file():
            return None, f"attachment not found: {path}"
        size = path.stat().st_size
        if size > cfg.max_outgoing_attachment_bytes:
            return None, f"attachment too large: {path} ({size} bytes > {cfg.max_outgoing_attachment_bytes})"
        data = path.read_bytes()
    except OSError as exc:
        return None, f"attachment read failed: {path}: {exc}"
    guessed = mimetypes.guess_type(str(path))[0] or "application/octet-stream"
    if "/" in guessed:
        maintype, subtype = guessed.split("/", 1)
    else:
        maintype, subtype = "application", "octet-stream"
    if path.suffix.lower() == ".json":
        maintype, subtype = "application", "json"
    return ReplyAttachment(filename or safe_filename(path.name), data, maintype, subtype), f"attached: {filename or path.name} ({len(data)} bytes)"


def build_reply(cfg: Config, original: Message, session: str, body: str, stderr_text: str, ok: bool) -> EmailMessage:
    sender = message_sender(original)
    reply_to = parseaddr(original.get("Reply-To") or original.get("From") or sender)[1]
    dest = cfg.mail_to or reply_to
    subject = original.get("Subject", f"tc:{{{session}}}")

    msg = EmailMessage()
    msg["From"] = cfg.mail_from or cfg.smtp_user or cfg.imap_user
    msg["To"] = dest
    msg["Subject"] = f"{cfg.reply_subject_prefix} {subject}"
    msg["Message-ID"] = make_msgid(domain=(msg["From"].split("@")[-1] if "@" in msg["From"] else None))
    if original.get("Message-ID"):
        msg["In-Reply-To"] = original.get("Message-ID")
        msg["References"] = original.get("Message-ID")

    status = "success" if ok else "error"
    text = [
        f"ttychatter.mailbot {status}",
        f"session: {session}",
        "",
        body or "(empty response)",
    ]
    if cfg.include_ttychatter_stderr and stderr_text:
        text.extend(["", "--- ttychatter stderr ---", stderr_text])
    msg.set_content("\n".join(text).rstrip() + "\n")
    return msg


def build_control_reply(
    cfg: Config,
    original: Message,
    command: str,
    body: str,
    ok: bool,
    attachments: Optional[List[ReplyAttachment]] = None,
) -> EmailMessage:
    sender = message_sender(original)
    reply_to = parseaddr(original.get("Reply-To") or original.get("From") or sender)[1]
    dest = cfg.mail_to or reply_to
    subject = original.get("Subject", f"tc:{{{cfg.mailbot_subject_token}}}")

    msg = EmailMessage()
    msg["From"] = cfg.mail_from or cfg.smtp_user or cfg.imap_user
    msg["To"] = dest
    msg["Subject"] = f"{cfg.reply_subject_prefix} {subject}"
    msg["Message-ID"] = make_msgid(domain=(msg["From"].split("@")[-1] if "@" in msg["From"] else None))
    if original.get("Message-ID"):
        msg["In-Reply-To"] = original.get("Message-ID")
        msg["References"] = original.get("Message-ID")

    status = "success" if ok else "error"
    text = [
        f"ttychatter.mailbot {status}",
        f"command: {command or 'help'}",
        "",
        body or "(empty response)",
    ]
    msg.set_content("\n".join(text).rstrip() + "\n")
    for attachment in attachments or []:
        msg.add_attachment(
            attachment.data,
            maintype=attachment.maintype,
            subtype=attachment.subtype,
            filename=safe_filename(attachment.filename),
        )
    return msg


def is_mailbot_request(session: str, cfg: Config) -> bool:
    token = (session or "").strip()
    prefix = (cfg.mailbot_subject_token or "MAILBOT").strip()
    if not token or not prefix:
        return False
    lower = token.lower()
    prefix_lower = prefix.lower()
    return lower == prefix_lower or lower.startswith(prefix_lower + ":")


def first_command_line_from_body(body: str) -> str:
    for raw in (body or "").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line == "--":
            break
        if line.startswith(">"):
            continue
        return line
    return ""


def mailbot_command_line(session: str, body: str, cfg: Config) -> str:
    token = (session or "").strip()
    prefix = (cfg.mailbot_subject_token or "MAILBOT").strip()
    prefix_lower = prefix.lower()
    lower = token.lower()
    if lower.startswith(prefix_lower + ":"):
        command = token[len(prefix) + 1 :].strip()
        if command:
            body_line = first_command_line_from_body(body)
            body_arg_commands = {
                "models", "routers", "search", "search-session", "rename-session",
                "favorite-model", "unfavorite-model", "test-model", "show-memory",
                "clear-memory", "tty-set", "tty-unset",
            }
            try:
                parsed = shlex.split(command)
            except ValueError:
                return command
            if len(parsed) == 1 and parsed[0].lower().replace("_", "-") in body_arg_commands and body_line:
                return command + " " + body_line
            return command
    return first_command_line_from_body(body) or "help"


def mailbot_command_help(cfg: Config) -> str:
    return f"""ttychatter.mailbot email commands

Address mail to the bot from an ALLOWED_FROM sender. Use one of these subject forms:

  {cfg.subject_prefix}:{{session-name}}
  {cfg.subject_prefix}:{{new}}
  {cfg.subject_prefix}:{{{cfg.config_subject_token}}}
  {cfg.subject_prefix}:{{{cfg.mailbot_subject_token}:command args}}
  {cfg.subject_prefix}:{{{cfg.mailbot_subject_token}}} with the command on the first nonempty body line

Chat:
  Any ordinary subject token is treated as a ttychatter session name. Attachments are passed through to ttychatter when PROCESS_ATTACHMENTS=1.

Mailbot/config:
  help                         Show this command list
  manpage | manual             Return the built-in roff manpage as an attachment
  config                       Export mailbot config; same as {cfg.subject_prefix}:{{{cfg.config_subject_token}}}

Sessions:
  sessions                     List session names with head/tail log previews
  search TEXT                  Search all session logs
  search-session SESSION TEXT  Search one session log
  rename-session OLD NEW       Rename a session log

Model cache and model list:
  update-models                Run ttychatter.python --update-models and attach models-cache.json
  models [model-list flags]    Run ttychatter.python --models with safe filter flags
  routers [model-list flags]   Run ttychatter.python --routers with safe filter flags

Model favorites/tests:
  favorites                    Run ttychatter.python --favorites
  favorite-model MODEL         Run ttychatter.python --favorite-model MODEL
  unfavorite-model MODEL       Run ttychatter.python --unfavorite-model MODEL
  test-model MODEL             Run ttychatter.python --test-model MODEL

Ttychatter utilities:
  ttyconfig                    Run ttychatter.python --config
  ttydoctor                    Run ttychatter.python --doctor
  credits                      Run ttychatter.python --credits
  show-memory SESSION          Run ttychatter.python --show-memory SESSION
  clear-memory SESSION         Run ttychatter.python --clear-memory SESSION
  tty-set KEY VALUE            Run ttychatter.python --set KEY VALUE
  tty-unset KEY                Run ttychatter.python --unset KEY

Safe model-list flags:
  --all --sort VALUE --modellist-sort-tokens --min-input-tokens N --min-output-tokens N
  --hide-preview --show-preview --require-tokens --allow-missing-tokens --model-type VALUE
"""


def embedded_mailbot_manpage(cfg: Optional[Config] = None) -> str:
    subject_prefix = cfg.subject_prefix if cfg else "tc"
    config_token = cfg.config_subject_token if cfg else "CONFIG"
    mailbot_token = cfg.mailbot_subject_token if cfg else "MAILBOT"
    return textwrap.dedent(f'''\
    .TH TTYCHATTER.MAILBOT 1 "May 2026" "ttychatter.mailbot {VERSION}" "User Commands"
    .SH NAME
    ttychatter.mailbot \\- email transport adapter and remote control surface for ttychatter
    .SH SYNOPSIS
    .B ttychatter.mailbot
    [--config-file PATH] [--once|--daemon|--dry-run] [--doctor] [--list-candidates]
    .PP
    .B ttychatter.mailbot
    [--config] [--sessions|--list-sessions] [--manpage] [--update-models]
    .PP
    .B ttychatter.mailbot
    [--set KEY VALUE] [--unset KEY] [--set-imap-password [--gpg]] [--set-smtp-password [--gpg]]
    .SH DESCRIPTION
    ttychatter.mailbot reads unread IMAP mail, accepts only messages from ALLOWED_FROM,
    extracts a subject token, and either sends the message body to ttychatter.python
    or handles a local mailbot control command. Replies are sent by sendmail or SMTP.
    .PP
    Ordinary chat subjects use this form:
    .PP
    .B {subject_prefix}:{{session-name}}
    .PP
    The special session name "new" creates a timestamped session.
    .SH EMAIL COMMANDS
    Mailbot commands are addressed with either of these forms:
    .PP
    .B {subject_prefix}:{{{mailbot_token}:command args}}
    .PP
    or
    .PP
    .B {subject_prefix}:{{{mailbot_token}}}
    .PP
    with the command on the first nonempty line of the message body.
    .PP
    Supported commands:
    .TP
    .B help
    Return the email command summary.
    .TP
    .B manpage
    Return this roff manpage as an email attachment. Aliases: manual, get-manpage,
    fetch-manpage.
    .TP
    .B sessions
    List saved session names with short head and tail previews from each session log.
    .TP
    .B search TEXT
    Search all session logs for TEXT.
    .TP
    .B search-session SESSION TEXT
    Search a single session log for TEXT.
    .TP
    .B rename-session OLD NEW
    Rename a session log after sanitizing both names.
    .TP
    .B update-models
    Run ttychatter.python --update-models, then attach the resulting models-cache.json
    when the cache file is present and smaller than MAX_OUTGOING_ATTACHMENT_BYTES.
    .TP
    .B models [FLAGS]
    Run ttychatter.python --models. The safe model-list flags are --all, --sort VALUE,
    --modellist-sort-tokens, --min-input-tokens N, --min-output-tokens N, --hide-preview,
    --show-preview, --require-tokens, --allow-missing-tokens, and --model-type VALUE.
    .TP
    .B routers [FLAGS]
    Run ttychatter.python --routers with the same safe model-list flags.
    .TP
    .B favorites
    Return ttychatter's model favorites.
    .TP
    .B favorite-model MODEL
    Add MODEL to ttychatter's favorites file.
    .TP
    .B unfavorite-model MODEL
    Remove MODEL from ttychatter's favorites file.
    .TP
    .B test-model MODEL
    Run ttychatter.python --test-model MODEL.
    .TP
    .B ttyconfig
    Return ttychatter.python --config output.
    .TP
    .B ttydoctor
    Return ttychatter.python --doctor diagnostics.
    .TP
    .B show-memory SESSION
    Return ttychatter's memory snapshot for SESSION.
    .TP
    .B clear-memory SESSION
    Clear ttychatter's memory snapshot for SESSION.
    .TP
    .B tty-set KEY VALUE
    Run ttychatter.python --set KEY VALUE. Do not send secrets by email.
    .TP
    .B tty-unset KEY
    Run ttychatter.python --unset KEY.
    .TP
    .B credits
    Return ttychatter.python --credits output.
    .SH CONFIG COMMAND
    The subject
    .PP
    .B {subject_prefix}:{{{config_token}}}
    .PP
    exports the mailbot config as an attachment. Reply with an edited config
    attachment using the same subject to replace the config; a timestamped backup is kept.
    Plaintext IMAP_PASSWORD and SMTP_PASSWORD values are redacted on export.
    .SH CONFIGURATION KEYS
    Core keys include IMAP_HOST, IMAP_PORT, IMAP_SSL, IMAP_USER, IMAP_PASSWORD,
    IMAP_PASSWORD_GPG_FILE, SEND_METHOD, SENDMAIL_PATH, SMTP_HOST, SMTP_PORT,
    SMTP_SSL, SMTP_STARTTLS, SMTP_USER, SMTP_PASSWORD, SMTP_PASSWORD_GPG_FILE,
    MAIL_FROM, MAIL_TO, ALLOWED_FROM, SUBJECT_PREFIX, CONFIG_SUBJECT_TOKEN,
    MAILBOT_SUBJECT_TOKEN, POLL_INTERVAL, MAX_MESSAGES_PER_RUN, MAX_BODY_BYTES,
    MAX_ATTACHMENT_BYTES, PROCESS_ATTACHMENTS, MARK_SEEN, TTYCHATTER_COMMAND,
    TTYCHATTER_EXTRA_ARGS, TTYCHATTER_CONTROL_TIMEOUT, MAILBOT_MANPAGE_FILE,
    SESSION_DIR, ATTACHMENT_DIR,
    MODEL_CACHE_FILE, WORK_DIR, PROCESSED_DB, SESSION_LIST_HEAD_LINES,
    SESSION_LIST_TAIL_LINES, SESSION_LIST_MAX_SESSIONS, SESSION_PREVIEW_LINE_CHARS,
    SESSION_SEARCH_MAX_MATCHES, MAX_COMMAND_OUTPUT_BYTES, MAX_OUTGOING_ATTACHMENT_BYTES,
    REPLY_SUBJECT_PREFIX, and INCLUDE_TTYCHATTER_STDERR.
    .SH SECURITY
    ALLOWED_FROM is required. The mailbot should use a private mailbox and a distinct
    MAIL_FROM address where practical. Commands are not shell-expanded, but email is
    not a good channel for secrets; use the local --set-imap-password and
    --set-smtp-password commands for passwords.
    .SH FILES
    Default config: ~/.config/ttychatter/mailbot/config
    .PP
    Default processed-message database: ~/.local/state/ttychatter/mailbot/processed-ids.txt
    .PP
    Default session directory: ~/.local/share/ttychatter/openrouter/sessions
    .PP
    Default model cache: ~/.config/ttychatter/openrouter/models-cache.json
    .SH EXIT STATUS
    0 indicates success. Nonzero status indicates configuration, IMAP, SMTP,
    sendmail, or ttychatter subprocess failure.
    ''').strip() + "\n"


def mailbot_manpage_candidates(cfg: Optional[Config] = None) -> List[Path]:
    """Return external manpage candidates, most specific first.

    The mailbot can still fall back to its compiled-in manpage.  This lookup is
    intentionally simple so a packaged install can keep documentation in normal
    man(1) locations, while a single-user install can keep it beside the config
    or beside the executable.
    """
    candidates: List[Path] = []

    def add(path: Optional[Path]) -> None:
        if path is None:
            return
        try:
            path = path.expanduser().resolve()
        except Exception:
            path = path.expanduser()
        if path not in candidates:
            candidates.append(path)

    if cfg and cfg.mailbot_manpage_file:
        add(cfg.mailbot_manpage_file)

    if cfg:
        add(cfg.config_file.parent / "ttychatter.mailbot.1")
        add(cfg.config_file.parent / "ttychatter.mailbot.1.gz")

    for base in [Path.cwd(), Path(__file__).resolve().parent, Path(sys.argv[0]).resolve().parent]:
        add(base / "ttychatter.mailbot.1")
        add(base / "ttychatter.mailbot.1.gz")

    xdg_data_home = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share")))
    for man_dir in [
        xdg_data_home / "man" / "man1",
        Path.home() / ".local" / "share" / "man" / "man1",
        Path("/usr/local/share/man/man1"),
        Path("/usr/local/man/man1"),
        Path("/usr/share/man/man1"),
    ]:
        add(man_dir / "ttychatter.mailbot.1")
        add(man_dir / "ttychatter.mailbot.1.gz")

    return candidates


def read_manpage_file(path: Path) -> str:
    if path.suffix == ".gz":
        with gzip.open(path, "rt", encoding="utf-8", errors="replace") as fh:
            return fh.read().rstrip() + "\n"
    return path.read_text(encoding="utf-8", errors="replace").rstrip() + "\n"


def external_mailbot_manpage(cfg: Optional[Config] = None) -> Optional[str]:
    for path in mailbot_manpage_candidates(cfg):
        try:
            if path.is_file():
                return read_manpage_file(path)
        except OSError:
            continue
    return None


def mailbot_manpage(cfg: Optional[Config] = None) -> str:
    return external_mailbot_manpage(cfg) or embedded_mailbot_manpage(cfg)



def validate_model_list_args(args: List[str]) -> Tuple[bool, List[str], str]:
    no_value = {
        "--all",
        "--modellist-sort-tokens",
        "--hide-preview",
        "--show-preview",
        "--require-tokens",
        "--allow-missing-tokens",
    }
    with_value = {"--sort", "--min-input-tokens", "--min-output-tokens", "--model-type"}
    safe: List[str] = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg in no_value:
            safe.append(arg)
            i += 1
            continue
        if arg in with_value:
            if i + 1 >= len(args):
                return False, [], f"{arg} requires a value"
            value = args[i + 1]
            if value.startswith("--"):
                return False, [], f"{arg} requires a value"
            safe.extend([arg, value])
            i += 2
            continue
        return False, [], f"unsafe or unsupported model-list flag: {arg}"
    return True, safe, ""


def dispatch_mailbot_command(cfg: Config, command_line: str, dry_run: bool = False) -> Tuple[bool, str, List[ReplyAttachment]]:
    try:
        tokens = shlex.split(command_line or "help")
    except ValueError as exc:
        return False, f"could not parse command: {exc}\n", []
    if not tokens:
        tokens = ["help"]
    command = tokens[0].strip().lower().replace("_", "-")
    args = tokens[1:]
    aliases = {
        "?": "help",
        "manual": "manpage",
        "get-manpage": "manpage",
        "fetch-manpage": "manpage",
        "list": "sessions",
        "list-sessions": "sessions",
        "session-list": "sessions",
        "update-model-list": "update-models",
        "update-model-cache": "update-models",
        "model-cache": "update-models",
        "router-list": "routers",
        "faves": "favorites",
        "unbookmark-model": "unfavorite-model",
        "doctor": "ttydoctor",
        "ttyconfig-summary": "ttyconfig",
    }
    command = aliases.get(command, command)

    if command == "help":
        return True, mailbot_command_help(cfg), []

    if command == "manpage":
        data = mailbot_manpage(cfg).encode("utf-8")
        body = "Attached: ttychatter.mailbot.1\nUse: man ./ttychatter.mailbot.1\n"
        return True, body, [ReplyAttachment("ttychatter.mailbot.1", data, "text", "plain")]

    if command == "config":
        return True, "Attached: ttychatter-mailbot-config.txt\n", [ReplyAttachment("ttychatter-mailbot-config.txt", public_config_text(cfg).encode("utf-8"), "text", "plain")]

    if command == "sessions":
        return True, format_session_list_with_previews(cfg), []

    if command == "search":
        query = " ".join(args).strip()
        return True, search_session_logs(cfg, query), []

    if command == "search-session":
        if len(args) < 2:
            return False, "usage: search-session SESSION TEXT\n", []
        return True, search_session_logs(cfg, " ".join(args[1:]), session=args[0]), []

    if command == "rename-session":
        if len(args) != 2:
            return False, "usage: rename-session OLD NEW\n", []
        if dry_run:
            return True, f"[dry-run] would rename session: {args[0]} -> {args[1]}\n", []
        try:
            return True, rename_session_file(cfg, args[0], args[1]), []
        except Exception as exc:
            return False, f"rename-session failed: {exc}\n", []

    if command == "update-models":
        if args:
            return False, "usage: update-models\n", []
        if dry_run:
            return True, f"[dry-run] would run: {cfg.ttychatter_command} --update-models\n", []
        rc, stdout, stderr_text = run_ttychatter_cli(cfg, ["--update-models"])
        output = format_cli_result("ttychatter.python --update-models", rc, stdout, stderr_text, cfg.max_command_output_bytes)
        cache_path = parse_updated_model_cache_path("\n".join([stdout, stderr_text])) or cfg.model_cache_file
        attachment, note = read_file_attachment(cache_path, cfg, "models-cache.json")
        output += f"\nmodel_cache_file: {cache_path}\n{note}\n"
        return rc == 0 and attachment is not None, output, [attachment] if attachment else []

    if command in {"models", "routers"}:
        ok, safe_args, reason = validate_model_list_args(args)
        if not ok:
            return False, reason + "\n", []
        cli = ["--models" if command == "models" else "--routers"] + safe_args
        if dry_run:
            return True, "[dry-run] would run: " + " ".join([cfg.ttychatter_command] + cli) + "\n", []
        rc, stdout, stderr_text = run_ttychatter_cli(cfg, cli)
        body = format_cli_result("ttychatter.python " + " ".join(cli), rc, stdout, stderr_text, cfg.max_command_output_bytes)
        attachments: List[ReplyAttachment] = []
        if stdout and len(stdout.encode("utf-8", errors="replace")) > cfg.max_command_output_bytes // 2:
            attachments.append(ReplyAttachment(f"ttychatter-{command}.txt", stdout.encode("utf-8"), "text", "plain"))
        return rc == 0, body, attachments

    fixed_tty_commands = {
        "favorites": ["--favorites"],
        "ttyconfig": ["--config"],
        "ttydoctor": ["--doctor"],
        "credits": ["--credits"],
    }
    if command in fixed_tty_commands:
        if args:
            return False, f"usage: {command}\n", []
        if dry_run:
            return True, "[dry-run] would run: " + " ".join([cfg.ttychatter_command] + fixed_tty_commands[command]) + "\n", []
        rc, stdout, stderr_text = run_ttychatter_cli(cfg, fixed_tty_commands[command])
        return rc == 0, format_cli_result("ttychatter.python " + " ".join(fixed_tty_commands[command]), rc, stdout, stderr_text, cfg.max_command_output_bytes), []

    single_arg_tty_commands = {
        "favorite-model": "--favorite-model",
        "unfavorite-model": "--unfavorite-model",
        "test-model": "--test-model",
        "show-memory": "--show-memory",
        "clear-memory": "--clear-memory",
        "tty-unset": "--unset",
    }
    if command in single_arg_tty_commands:
        if len(args) != 1:
            return False, f"usage: {command} ARG\n", []
        cli = [single_arg_tty_commands[command], args[0]]
        if dry_run:
            return True, "[dry-run] would run: " + " ".join([cfg.ttychatter_command] + cli) + "\n", []
        rc, stdout, stderr_text = run_ttychatter_cli(cfg, cli)
        return rc == 0, format_cli_result("ttychatter.python " + " ".join(cli), rc, stdout, stderr_text, cfg.max_command_output_bytes), []

    if command == "tty-set":
        if len(args) < 2:
            return False, "usage: tty-set KEY VALUE\n", []
        key = args[0]
        value = " ".join(args[1:])
        if key.upper() in {"OPENROUTER_API_KEY", "API_KEY"}:
            return False, "refusing to set API keys by email; use ttychatter.python --set-api-key locally\n", []
        cli = ["--set", key, value]
        if dry_run:
            return True, "[dry-run] would run: " + " ".join([cfg.ttychatter_command, "--set", key, "VALUE"]) + "\n", []
        rc, stdout, stderr_text = run_ttychatter_cli(cfg, cli)
        return rc == 0, format_cli_result(f"ttychatter.python --set {key} VALUE", rc, stdout, stderr_text, cfg.max_command_output_bytes), []

    return False, f"unknown mailbot command: {tokens[0]}\n\n" + mailbot_command_help(cfg), []


def fetch_message_peek(conn: imaplib.IMAP4, uid: bytes) -> Optional[Message]:
    """Fetch an unread candidate without consuming its unread state.

    The single most important safety rule in this mailbot is: inspection must
    not mutate the mailbox.  IMAP BODY[] and RFC822 fetches commonly set the
    \\Seen flag.  BODY.PEEK[] is the standard way to read enough of the message
    for parsing while leaving unread messages unread until ttychatter actually
    processes them successfully.
    """
    status, data = conn.uid("FETCH", uid, "(BODY.PEEK[])")
    if status != "OK" or not data or data[0] is None:
        return None
    raw = data[0][1]
    return email.message_from_bytes(raw, policy=email.policy.default)


def candidate_skip_reason(cfg: Config, msg: Message, uid: bytes, processed: set[str]) -> str:
    subject = msg.get("Subject", "")
    session = parse_session_from_subject(subject, cfg.subject_prefix)
    sender = message_sender(msg)
    msgid = msg.get("Message-ID", f"imap-uid-{uid.decode(errors='replace')}")
    if not session:
        return "skip: subject does not contain tc:{...}"
    if not allowed_sender(sender, cfg):
        return f"skip: sender not allowed ({sender or 'empty sender'}); allowed={','.join(cfg.allowed_from) or 'empty'}"
    if msgid in processed:
        return "skip: Message-ID already processed"
    if is_config_request(session, cfg):
        return "match: mailbot config command"
    if is_mailbot_request(session, cfg):
        return "match: mailbot control command"
    return "match: ttychatter session"


def process_config_message(cfg: Config, conn: imaplib.IMAP4, uid: bytes, msg: Message, dry_run: bool = False) -> bool:
    msgid = msg.get("Message-ID", f"imap-uid-{uid.decode(errors='replace')}")
    processed = load_processed(cfg.processed_db)
    if msgid in processed:
        return False

    found = find_config_attachment(msg, cfg.max_attachment_bytes)
    if found is None:
        body = (
            "ttychatter.mailbot config export\n"
            "\n"
            "Edit the attached config file and mail it back with the same subject tc:{CONFIG}.\n"
            "Plaintext password values are redacted; encrypted password file paths are preserved.\n"
        )
        reply = build_config_reply(cfg, msg, body, attachment_text=public_config_text(cfg))
    else:
        filename, text = found
        if dry_run:
            body = f"[dry-run] would replace config from attachment: {filename}\n"
        else:
            backup = apply_config_attachment(cfg, text)
            body = f"updated config from attachment: {filename}\nbackup: {backup}\n"
        reply = build_config_reply(cfg, msg, body)

    if dry_run:
        print(reply.as_string())
    else:
        send_reply(cfg, reply)
        remember_processed(cfg.processed_db, msgid)
        if cfg.mark_seen:
            conn.uid("STORE", uid, "+FLAGS", "(\\Seen)")
    return True


def process_mailbot_command_message(cfg: Config, conn: imaplib.IMAP4, uid: bytes, msg: Message, session: str, dry_run: bool = False) -> bool:
    msgid = msg.get("Message-ID", f"imap-uid-{uid.decode(errors='replace')}")
    processed = load_processed(cfg.processed_db)
    if msgid in processed:
        return False

    body = extract_body(msg, cfg.max_body_bytes)
    command_line = mailbot_command_line(session, body, cfg)
    ok, output, attachments = dispatch_mailbot_command(cfg, command_line, dry_run=dry_run)
    reply = build_control_reply(cfg, msg, command_line, output, ok, attachments)

    if dry_run:
        print(reply.as_string())
    else:
        send_reply(cfg, reply)
        remember_processed(cfg.processed_db, msgid)
        if cfg.mark_seen:
            conn.uid("STORE", uid, "+FLAGS", "(\\Seen)")
    return True


def process_message(cfg: Config, conn: imaplib.IMAP4, uid: bytes, dry_run: bool = False) -> bool:
    msg = fetch_message_peek(conn, uid)
    if msg is None:
        return False
    msgid = msg.get("Message-ID", f"imap-uid-{uid.decode(errors='replace')}")
    subject = msg.get("Subject", "")
    session = parse_session_from_subject(subject, cfg.subject_prefix)
    if not session:
        return False
    if not allowed_sender(message_sender(msg), cfg):
        return False

    processed = load_processed(cfg.processed_db)
    if msgid in processed:
        return False
    if is_config_request(session, cfg):
        return process_config_message(cfg, conn, uid, msg, dry_run=dry_run)
    if is_mailbot_request(session, cfg):
        return process_mailbot_command_message(cfg, conn, uid, msg, session, dry_run=dry_run)

    body = extract_body(msg, cfg.max_body_bytes)
    if not body:
        body = "(empty email body)"

    with tempfile.TemporaryDirectory(prefix="ttychatter-mailbot-", dir=str(cfg.work_dir)) as tmp:
        tempdir = Path(tmp)
        attachments = save_attachments(msg, tempdir, cfg.max_attachment_bytes) if cfg.process_attachments else []
        rc, output, stderr_text = (0, "[dry-run] would call ttychatter here", "")
        if not dry_run:
            rc, output, stderr_text = run_ttychatter(cfg, session, body, attachments)
        ok = rc == 0
        reply = build_reply(cfg, msg, session, output if ok else stderr_text or output, stderr_text, ok)
        if not dry_run:
            send_reply(cfg, reply)
            remember_processed(cfg.processed_db, msgid)
            if cfg.mark_seen:
                conn.uid("STORE", uid, "+FLAGS", "(\\Seen)")
        else:
            print(reply.as_string())
    return True


def list_candidates(cfg: Config) -> int:
    """Print unread candidates and skip reasons without changing mail state."""
    cfg.work_dir.mkdir(parents=True, exist_ok=True)
    conn = imap_connect(cfg)
    try:
        status, data = conn.uid("SEARCH", None, "UNSEEN")
        if status != "OK":
            print("IMAP search failed", file=sys.stderr)
            return 1
        uids = (data[0] or b"").split()
        processed = load_processed(cfg.processed_db)
        if not uids:
            print("no unread candidates")
            return 0
        for uid in uids:
            msg = fetch_message_peek(conn, uid)
            if msg is None:
                print(f"uid={uid.decode(errors='replace')} skip: fetch failed")
                continue
            sender = message_sender(msg)
            subject = msg.get("Subject", "")
            msgid = msg.get("Message-ID", f"imap-uid-{uid.decode(errors='replace')}")
            reason = candidate_skip_reason(cfg, msg, uid, processed)
            print(f"uid={uid.decode(errors='replace')} from={sender} subject={subject!r} message_id={msgid!r} {reason}")
    finally:
        try:
            conn.logout()
        except Exception:
            pass
    return 0


def run_once(cfg: Config, dry_run: bool = False) -> int:
    cfg.work_dir.mkdir(parents=True, exist_ok=True)
    cfg.processed_db.parent.mkdir(parents=True, exist_ok=True)
    cfg.session_dir.mkdir(parents=True, exist_ok=True)
    cfg.attachment_dir.mkdir(parents=True, exist_ok=True)

    conn = imap_connect(cfg)
    processed_count = 0
    try:
        status, data = conn.uid("SEARCH", None, "UNSEEN")
        if status != "OK":
            print("IMAP search failed", file=sys.stderr)
            return 1
        uids = (data[0] or b"").split()
        for uid in uids[: cfg.max_messages_per_run]:
            if process_message(cfg, conn, uid, dry_run=dry_run):
                processed_count += 1
    finally:
        try:
            conn.logout()
        except Exception:
            pass
    print(f"processed messages: {processed_count}")
    return 0


def run_daemon(cfg: Config, dry_run: bool = False) -> int:
    while True:
        try:
            run_once(cfg, dry_run=dry_run)
        except KeyboardInterrupt:
            return 0
        except Exception as exc:
            print(f"mailbot error: {exc}", file=sys.stderr)
        time.sleep(cfg.poll_interval)


# ---------------------------------------------------------------------------
# CLI utilities
# ---------------------------------------------------------------------------


def doctor(cfg: Config) -> int:
    checks: List[Tuple[str, bool, str]] = []
    checks.append(("config file", cfg.config_file.exists(), str(cfg.config_file)))
    checks.append(("imap host", bool(cfg.imap_host), cfg.imap_host))
    checks.append(("imap user", bool(cfg.imap_user), cfg.imap_user or "missing"))
    checks.append(("allowed from", bool(cfg.allowed_from), ", ".join(cfg.allowed_from) or "missing"))
    checks.append(("mail from", bool(cfg.mail_from or cfg.smtp_user or cfg.imap_user), cfg.mail_from or cfg.smtp_user or cfg.imap_user or "missing"))
    checks.append(("gpg", gpg_available(), "found" if gpg_available() else "not found"))
    checks.append(("ttychatter command", bool(cfg.ttychatter_command), cfg.ttychatter_command))
    checks.append(("model cache file", True, str(cfg.model_cache_file)))
    found_manpage = next((p for p in mailbot_manpage_candidates(cfg) if p.is_file()), None)
    checks.append(("mailbot manpage", True, str(found_manpage) if found_manpage else "embedded fallback"))
    for label, path in [
        ("session dir", cfg.session_dir),
        ("attachment dir", cfg.attachment_dir),
        ("model cache dir", cfg.model_cache_file.parent),
        ("work dir", cfg.work_dir),
        ("processed db dir", cfg.processed_db.parent),
    ]:
        try:
            path.mkdir(parents=True, exist_ok=True)
            writable = os.access(path, os.W_OK)
        except Exception:
            writable = False
        checks.append((label, writable, str(path)))

    rc = 0
    for name, ok, detail in checks:
        print(f"{'OK' if ok else 'FAIL'}\t{name}\t{detail}")
        if not ok and name in {"imap user", "allowed from", "mail from", "ttychatter command"}:
            rc = 1
    return rc


def print_config(cfg: Config) -> None:
    # Never print password values.  Showing that a password source exists is
    # enough for diagnostics and avoids accidental secret disclosure in forum
    # posts or bug reports.
    print(f"CONFIG_FILE={cfg.config_file}")
    print(f"IMAP_HOST={cfg.imap_host}")
    print(f"IMAP_PORT={cfg.imap_port}")
    print(f"IMAP_SSL={int(cfg.imap_ssl)}")
    print(f"IMAP_USER={cfg.imap_user}")
    print(f"IMAP_PASSWORD={'set' if cfg.imap_password else 'unset'}")
    print(f"IMAP_PASSWORD_GPG_FILE={cfg.imap_password_gpg_file}")
    print(f"SEND_METHOD={cfg.send_method}")
    print(f"SMTP_HOST={cfg.smtp_host}")
    print(f"SMTP_PORT={cfg.smtp_port}")
    print(f"MAIL_FROM={cfg.mail_from}")
    print(f"ALLOWED_FROM={','.join(cfg.allowed_from)}")
    print(f"TTYCHATTER_COMMAND={cfg.ttychatter_command}")
    print(f"TTYCHATTER_CONTROL_TIMEOUT={cfg.ttychatter_control_timeout}")
    print(f"SESSION_DIR={cfg.session_dir}")
    print(f"ATTACHMENT_DIR={cfg.attachment_dir}")
    print(f"MODEL_CACHE_FILE={cfg.model_cache_file}")
    print(f"SUBJECT_PREFIX={cfg.subject_prefix}")
    print(f"CONFIG_SUBJECT_TOKEN={cfg.config_subject_token}")
    print(f"MAILBOT_SUBJECT_TOKEN={cfg.mailbot_subject_token}")
    print(f"MAILBOT_MANPAGE_FILE={cfg.mailbot_manpage_file or ''}")
    print(f"CONFIG_UPDATE_REQUIRES_ATTACHMENT={int(cfg.config_update_requires_attachment)}")
    print(f"SESSION_LIST_HEAD_LINES={cfg.session_list_head_lines}")
    print(f"SESSION_LIST_TAIL_LINES={cfg.session_list_tail_lines}")
    print(f"SESSION_LIST_MAX_SESSIONS={cfg.session_list_max_sessions}")
    print(f"SESSION_PREVIEW_LINE_CHARS={cfg.session_preview_line_chars}")
    print(f"SESSION_SEARCH_MAX_MATCHES={cfg.session_search_max_matches}")
    print(f"MAX_COMMAND_OUTPUT_BYTES={cfg.max_command_output_bytes}")
    print(f"MAX_OUTGOING_ATTACHMENT_BYTES={cfg.max_outgoing_attachment_bytes}")


def set_password_command(cfg: Config, which: str, encrypted: bool) -> int:
    if encrypted and not gpg_available():
        print("gpg not found", file=sys.stderr)
        return 1
    secret = getpass.getpass(f"Enter {which.upper()} password: ")
    if not secret:
        print("empty password refused", file=sys.stderr)
        return 1
    if which == "imap":
        if encrypted:
            encrypt_secret_to_gpg(cfg.imap_password_gpg_file, secret)
            unset_config_value(cfg.config_file, "IMAP_PASSWORD")
            set_config_value(cfg.config_file, "IMAP_PASSWORD_GPG_FILE", str(cfg.imap_password_gpg_file))
            print(f"saved encrypted IMAP password to {cfg.imap_password_gpg_file}")
        else:
            set_config_value(cfg.config_file, "IMAP_PASSWORD", secret)
            print("saved IMAP_PASSWORD in plaintext config")
    else:
        if encrypted:
            encrypt_secret_to_gpg(cfg.smtp_password_gpg_file, secret)
            unset_config_value(cfg.config_file, "SMTP_PASSWORD")
            set_config_value(cfg.config_file, "SMTP_PASSWORD_GPG_FILE", str(cfg.smtp_password_gpg_file))
            print(f"saved encrypted SMTP password to {cfg.smtp_password_gpg_file}")
        else:
            set_config_value(cfg.config_file, "SMTP_PASSWORD", secret)
            print("saved SMTP_PASSWORD in plaintext config")
    return 0


def help_text() -> str:
    return f"""{PROGRAM} - email transport adapter and remote control surface for ttychatter

USAGE
    {PROGRAM} --once
    {PROGRAM} --daemon
    {PROGRAM} --doctor
    {PROGRAM} --list-candidates
    {PROGRAM} --sessions
    {PROGRAM} --manpage
    {PROGRAM} --update-models
    {PROGRAM} --config
    {PROGRAM} --set KEY VALUE
    {PROGRAM} --unset KEY
    {PROGRAM} --set-imap-password [--gpg]
    {PROGRAM} --set-smtp-password [--gpg]
    {PROGRAM} --version
    {PROGRAM} --help

MAIL SUBJECTS
    tc:{{session-name}}              Send body to ttychatter session
    tc:{{new}}                       Start a timestamped ttychatter session
    tc:{{CONFIG}}                    Export/apply mailbot config attachment
    tc:{{MAILBOT:help}}              Return mailbot command help
    tc:{{MAILBOT:sessions}}          List sessions with head/tail previews
    tc:{{MAILBOT:update-models}}     Refresh ttychatter model cache and attach JSON
    tc:{{MAILBOT:manpage}}           Return the mailbot manpage attachment

DESCRIPTION
    Reads unread mail from IMAP, extracts messages whose subject contains
    tc:{{session}}, sends ordinary messages to ttychatter, and handles explicit
    tc:{{MAILBOT:...}} control commands by email.

SECURITY
    ALLOWED_FROM is required. Mail from other senders is ignored. Commands are
    allowlisted and are not shell-expanded. Do not send secrets by email.
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--config-file", default=str(DEFAULT_CONFIG_FILE))
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--daemon", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--doctor", action="store_true")
    parser.add_argument("--list-candidates", action="store_true")
    parser.add_argument("--sessions", action="store_true")
    parser.add_argument("--list-sessions", action="store_true")
    parser.add_argument("--manpage", action="store_true")
    parser.add_argument("--update-models", action="store_true")
    parser.add_argument("--config", action="store_true")
    parser.add_argument("--set", nargs=2, metavar=("KEY", "VALUE"))
    parser.add_argument("--unset", metavar="KEY")
    parser.add_argument("--set-imap-password", action="store_true")
    parser.add_argument("--set-smtp-password", action="store_true")
    parser.add_argument("--gpg", action="store_true")
    parser.add_argument("--version", action="store_true")
    parser.add_argument("--help", "-h", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.help:
        print(help_text())
        return 0
    if args.version:
        print(f"{PROGRAM} version {VERSION}")
        return 0

    cfg = load_config(expand_path(args.config_file))

    if args.manpage:
        print(mailbot_manpage(cfg), end="")
        return 0
    if args.sessions or args.list_sessions:
        print(format_session_list_with_previews(cfg), end="")
        return 0
    if args.update_models:
        ok, body, _attachments = dispatch_mailbot_command(cfg, "update-models", dry_run=args.dry_run)
        print(body, end="")
        return 0 if ok else 1

    if args.set:
        set_config_value(cfg.config_file, args.set[0], args.set[1])
        return 0
    if args.unset:
        unset_config_value(cfg.config_file, args.unset)
        return 0
    if args.config:
        print_config(cfg)
        return 0
    if args.set_imap_password:
        return set_password_command(cfg, "imap", args.gpg)
    if args.set_smtp_password:
        return set_password_command(cfg, "smtp", args.gpg)
    if args.doctor:
        return doctor(cfg)
    if args.list_candidates:
        return list_candidates(cfg)
    if args.daemon:
        try:
            return run_daemon(cfg, dry_run=args.dry_run)
        except imaplib.IMAP4.error as exc:
            print(f"IMAP error: {exc}", file=sys.stderr)
            return 1
        except RuntimeError as exc:
            print(f"mailbot error: {exc}", file=sys.stderr)
            return 1
    if args.once:
        try:
            return run_once(cfg, dry_run=args.dry_run)
        except imaplib.IMAP4.error as exc:
            print(f"IMAP error: {exc}", file=sys.stderr)
            return 1
        except RuntimeError as exc:
            print(f"mailbot error: {exc}", file=sys.stderr)
            return 1

    print(help_text(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
