import logging
import time

import httpx
from telegram import InlineKeyboardButton as B, InlineKeyboardMarkup as M, Update
from telegram.error import BadRequest
from telegram.ext import Application, CallbackQueryHandler, CommandHandler, ContextTypes

import config

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(message)s",
    handlers=[logging.FileHandler("bot.log", encoding="utf-8"), logging.StreamHandler()],
)
logging.getLogger("httpx").setLevel(logging.WARNING)  # httpx logs URLs with the bot token
log = logging.getLogger("ipr")

MENU = M([
    [B("Device Info", callback_data="info"), B("Battery", callback_data="batt")],
    [B("System", callback_data="sys"), B("Network", callback_data="net")],
    [B("Uptime", callback_data="up"), B("Status", callback_data="ping")],
    [B("Reboot", callback_data="reboot"), B("Shutdown", callback_data="shutdown")],
])

yn = lambda b: "yes" if b else "no"


def gb(n):
    return f"{n / 2**30:.1f} GB"


def mb(n):
    return f"{n / 2**20:.0f} MB"


def dur(s):
    d, s = divmod(int(s), 86400)
    h, s = divmod(s, 3600)
    return f"{d}d {h}h {s // 60}m"


def fmt_info(d):
    return (f"Device: {d['model']} ({d['model_id']})\niOS: {d['ios']} ({d['build']})\n"
            f"CPU: {d['arch']}, {d['cores']} cores\n"
            f"Disk: {gb(d['disk_free'])} free of {gb(d['disk_total'])}")


def fmt_batt(d):
    t = f"{d['temp'] / 100:.1f}C" if d.get("temp", -1) >= 0 else "n/a"
    return (f"Battery: {d['percent']}%\nCharging: {yn(d['charging'])}\n"
            f"Plugged in: {yn(d['plugged'])}\nCycles: {d['cycles']}\nTemp: {t}")


def fmt_sys(d):
    pct = d["ram_used"] * 100 / d["ram_total"] if d["ram_total"] else 0
    return f"RAM: {mb(d['ram_used'])} / {mb(d['ram_total'])} ({pct:.0f}%)\nCPU load: {d['cpu_pct']}%"


def fmt_net(d):
    return (f"Wi-Fi: {yn(d['wifi'])}" + (f" ({d['ip']})" if d["ip"] else "") +
            f"\nCellular: {yn(d['cellular'])}\nInternet: {yn(d['internet'])}")


ROUTES = {
    "info": ("/info", fmt_info),
    "batt": ("/battery", fmt_batt),
    "sys": ("/system", fmt_sys),
    "net": ("/network", fmt_net),
    "up": ("/uptime", lambda d: f"Uptime: {dur(d['uptime'])}"),
    "ping": ("/ping", lambda d: "iPhone is online"),
}


async def phone(method, path, timeout=6.0):
    async with httpx.AsyncClient(timeout=timeout) as c:
        r = await c.request(method, config.PHONE_URL + path, headers={"X-Token": config.PHONE_TOKEN})
    return r.status_code, r.json()


_last = {}


def too_fast(uid):
    now = time.monotonic()
    if now - _last.get(uid, 0) < config.MIN_GAP:
        return True
    _last[uid] = now
    return False


async def edit(q, text):
    text += f"\n\nupdated {time.strftime('%H:%M:%S')}"
    try:
        await q.edit_message_text(text, reply_markup=MENU)
    except BadRequest as e:
        if "not modified" not in str(e).lower():
            raise


async def start(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    uid = update.effective_user.id
    if uid not in config.OWNER_IDS:
        log.warning("denied /start from %s", uid)
        return
    await update.message.reply_text("iPhoneRemote\nmain menu", reply_markup=MENU)


async def on_button(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    q = update.callback_query
    uid = q.from_user.id
    if uid not in config.OWNER_IDS:
        log.warning("denied button %s from %s", q.data, uid)
        await q.answer()
        return
    if too_fast(uid):
        await q.answer("slow down")
        return
    await q.answer()
    k = q.data
    log.info("action %s by %s", k, uid)
    try:
        if k in ROUTES:
            path, fmt = ROUTES[k]
            st, d = await phone("GET", path)
            text = fmt(d) if st == 200 and "error" not in d else f"error {st}: {d.get('error', '?')}"
        elif k in ("reboot", "shutdown"):
            await edit(q, f"{k}: confirm on the iPhone screen (30s)...")
            st, d = await phone("POST", "/" + k, timeout=45.0)
            text = {200: f"{k} confirmed, going down",
                    403: f"{k} declined or timed out on the device",
                    409: "another confirmation is pending"}.get(st, f"error {st}")
        else:
            return
    except (httpx.HTTPError, ValueError):
        text = "iPhone offline or unreachable"
    await edit(q, text)


_state = {"fails": 0, "online": None, "low": False, "chg": None}


async def notify(ctx, text):
    for uid in config.OWNER_IDS:
        await ctx.bot.send_message(uid, text)


async def poll(ctx: ContextTypes.DEFAULT_TYPE):
    s = _state
    try:
        st, b = await phone("GET", "/battery", timeout=5.0)
    except (httpx.HTTPError, ValueError):
        s["fails"] += 1
        if s["fails"] >= 2 and s["online"] is not False:
            s["online"] = False
            await notify(ctx, "iPhone went offline")
        return
    s["fails"] = 0
    if st != 200 or "percent" not in b:
        return
    if s["online"] is False:
        await notify(ctx, "iPhone is back online")
    s["online"] = True
    low = b["percent"] <= config.LOW_BATT and not b["charging"]
    if low and not s["low"]:
        await notify(ctx, f"low battery: {b['percent']}%")
    s["low"] = low
    if s["chg"] is not None and b["charging"] != s["chg"]:
        await notify(ctx, f"charging {'started' if b['charging'] else 'stopped'} ({b['percent']}%)")
    s["chg"] = b["charging"]


def main():
    app = Application.builder().token(config.BOT_TOKEN).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(CallbackQueryHandler(on_button))
    app.job_queue.run_repeating(poll, interval=config.POLL_SEC, first=5)
    app.run_polling()


if __name__ == "__main__":
    main()
