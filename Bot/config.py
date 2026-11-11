import os

# tiny .env loader (Bot/.env)
_p = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")
if os.path.exists(_p):
    for _l in open(_p, encoding="utf-8"):
        _l = _l.strip()
        if _l and not _l.startswith("#") and "=" in _l:
            _k, _v = _l.split("=", 1)
            os.environ.setdefault(_k.strip(), _v.strip())

BOT_TOKEN = os.environ["BOT_TOKEN"]
OWNER_IDS = {int(x) for x in os.environ["OWNER_IDS"].split(",") if x.strip()}
PHONE_URL = os.environ["PHONE_URL"].rstrip("/")   # http://192.168.1.50:8787
PHONE_TOKEN = os.environ["PHONE_TOKEN"]

POLL_SEC = 60      # background check for notifications
LOW_BATT = 20      # % threshold
MIN_GAP = 1.0      # seconds between button presses per user
