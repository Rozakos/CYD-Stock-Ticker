#pragma once

#include <Arduino.h>

#include <vector>

#include "../config.h"
#include "../settings/settings_store.h"

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef F
#define F(x) x
#endif

namespace web_admin_page {

inline const char PAGE_HEAD_P[] PROGMEM =
  "<!DOCTYPE html><html><head>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Rozakos Industries - Stock Ticker</title><style>"
  ":root{--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#e6edf3;"
  "--muted:#8b949e;--accent:#d63a3a;--action:#a31c1c}"
  "*{box-sizing:border-box}"
  "body{font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;"
  "max-width:520px;margin:0 auto;padding:0;background:var(--bg);"
  "color:var(--text);min-height:100vh;display:flex;flex-direction:column;overflow-x:hidden}"
  "main{flex:1;padding:4px 16px 16px}"
  ".brand{display:flex;align-items:center;gap:14px;padding:14px 16px;"
  "border-bottom:1px solid #3d1c1c;background:#110808}"
  ".pill{display:inline-flex;border:2px solid #5a3030;border-radius:20px;overflow:hidden}"
  ".pil-a{background:#c42525;color:white;font-size:12px;font-weight:700;"
  "letter-spacing:1.5px;padding:4px 10px;white-space:nowrap}"
  ".pil-b{color:var(--text);font-size:12px;font-weight:700;"
  "letter-spacing:1.5px;padding:4px 10px;white-space:nowrap}"
  ".tag{font-size:9px;letter-spacing:3px;color:var(--muted);margin-top:5px}"
  "h1{font-size:18px;margin:14px 0 4px;font-weight:600}"
  "h2{font-size:11px;margin:22px 0 8px;font-weight:600;color:var(--muted);"
  "text-transform:uppercase;letter-spacing:1.5px}"
  "table{width:100%;border-collapse:separate;border-spacing:0;"
  "background:var(--card);border:1px solid var(--border);border-radius:8px;"
  "overflow:hidden}"
  "th{padding:9px 12px;background:#1c2128;font-size:10px;"
  "text-transform:uppercase;letter-spacing:1.2px;color:var(--muted);"
  "text-align:left;font-weight:600}"
  "td{padding:10px 12px;border-top:1px solid var(--border)}"
  "input,button{padding:9px 11px;font-size:15px;border-radius:6px;"
  "border:1px solid var(--border);background:#0d1117;color:var(--text);"
  "font-family:inherit}"
  "input:focus{outline:none;border-color:var(--accent)}"
  "button{background:var(--action);border-color:var(--action);"
  "cursor:pointer;font-weight:600;color:white}"
  "button:hover{filter:brightness(1.18)}"
  "button.del{background:#c0392b;border-color:#c0392b;padding:6px 10px}"
  "form.inline{margin:0}"
  "form.add{display:flex;gap:8px;margin-top:8px;flex-wrap:wrap}"
  "form.add input{min-width:0}"
  "form.add input[name=symbol]{width:5em;text-transform:uppercase;flex-shrink:0}"
  "form.add button{flex-shrink:0}"
  ".empty{opacity:.55;font-style:italic;text-align:center}"
  ".banner{background:#2d0e0e;border:1px solid #8c1a1a;color:#ffb3b3;"
  "padding:10px 14px;border-radius:6px;margin-top:14px;font-size:14px}"
  ".banner a{color:var(--accent);font-weight:600}"
  ".topnav{display:flex;justify-content:space-between;align-items:center}"
  ".topnav a{color:var(--accent);text-decoration:none;font-size:14px;"
  "font-weight:500}"
  ".topnav a:hover{text-decoration:underline}"
  "label{display:block;margin:18px 0 6px;font-size:11px;color:var(--muted);"
  "text-transform:uppercase;letter-spacing:1.2px;font-weight:600}"
  ".set input{width:100%}"
  ".set button{width:100%;margin-top:18px;padding:11px}"
  ".hint{font-size:12px;color:var(--muted);margin-top:6px;line-height:1.5}"
  ".hint code{background:var(--card);padding:1px 5px;border-radius:3px;"
  "font-size:11px;color:var(--accent)}"
  "footer{margin-top:auto;padding:16px;border-top:1px solid #3d1c1c;"
  "text-align:center;font-size:12px;color:var(--muted);background:#110808}"
  "footer a{color:var(--muted);text-decoration:none;margin:0 6px}"
  "footer a:hover{color:var(--accent)}"
  "footer .dot{opacity:.35}"
  "@media(max-width:430px){main{padding:4px 10px 12px}"
  "td,th{padding:6px 7px}h1{font-size:16px}}"
  "</style></head><body>"
  "<header class=brand>"
  "<svg viewBox='0 0 100 122' width='36' height='44' aria-hidden='true'>"
  "<circle cx='28' cy='8' r='7' fill='#c42525'/>"
  "<circle cx='72' cy='8' r='7' fill='#c42525'/>"
  "<line x1='28' y1='14' x2='37' y2='28' stroke='#c42525' stroke-width='6' stroke-linecap='round'/>"
  "<line x1='72' y1='14' x2='63' y2='28' stroke='#c42525' stroke-width='6' stroke-linecap='round'/>"
  "<rect x='8' y='24' width='84' height='62' rx='20' fill='#c42525'/>"
  "<rect x='17' y='39' width='66' height='30' rx='9' fill='white'/>"
  "<circle cx='36' cy='54' r='10' fill='#c42525'/>"
  "<circle cx='32' cy='50' r='3.5' fill='white'/>"
  "<circle cx='64' cy='54' r='10' fill='#c42525'/>"
  "<circle cx='60' cy='50' r='3.5' fill='white'/>"
  "<rect x='41' y='86' width='18' height='12' rx='3' fill='#c42525'/>"
  "<rect x='33' y='98' width='34' height='14' rx='6' fill='#c42525'/>"
  "</svg>"
  "<div><div class=pill>"
  "<span class=pil-a>ROZAKOS</span><span class=pil-b>INDUSTRIES</span>"
  "</div><div class=tag>BUILD YOUR IDEAS</div></div>"
  "</header><main>";

inline const char PAGE_FOOT_P[] PROGMEM =
  "</main><footer>"
  "<div>Stock Ticker &middot; &copy; Rozakos Industries</div>"
  "<div style='margin-top:5px'>"
  "<a href='mailto:info@rozakos.com'>info@rozakos.com</a>"
  "<span class=dot>&middot;</span>"
  "<a href='tel:+306975590339'>+30 697 559 0339</a>"
  "</div></footer></body></html>";

inline const char DATALIST_HTML[] PROGMEM =
  "<datalist id=symbols>"
  "<option value=AAPL><option value=MSFT><option value=GOOG><option value=GOOGL>"
  "<option value=AMZN><option value=META><option value=NFLX><option value=TSLA>"
  "<option value=NVDA><option value=AMD><option value=INTC><option value=TSM>"
  "<option value=AVGO><option value=QCOM><option value=MU><option value=ASML>"
  "<option value=AMAT><option value=ARM><option value=MRVL><option value=ORCL>"
  "<option value=CRM><option value=ADBE><option value=NOW><option value=PANW>"
  "<option value=SNOW><option value=CRWD><option value=PLTR><option value=SHOP>"
  "<option value=IBM><option value=CSCO><option value=UBER><option value=ABNB>"
  "<option value=SPOT><option value=JPM><option value=BAC><option value=WFC>"
  "<option value=GS><option value=MS><option value=V><option value=MA>"
  "<option value=AXP><option value=PYPL><option value=COIN><option value=F>"
  "<option value=GM><option value=RIVN><option value=LCID><option value=WMT>"
  "<option value=COST><option value=TGT><option value=HD><option value=NKE>"
  "<option value=SBUX><option value=MCD><option value=DIS><option value=KO>"
  "<option value=PEP><option value=PG><option value=JNJ><option value=PFE>"
  "<option value=MRK><option value=LLY><option value=UNH><option value=ABBV>"
  "<option value=T><option value=VZ><option value=TMUS><option value=BA>"
  "<option value=CAT><option value=GE><option value=XOM><option value=CVX>"
  "<option value=SPY><option value=QQQ><option value=VOO><option value=VTI>"
  "<option value=IWM><option value=DIA><option value=GLD><option value=ARKK>"
  "<option value=SOXX><option value=SMH><option value=BABA><option value=BIDU>"
  "<option value=JD><option value=PDD><option value=MSTR>"
  "</datalist>";

inline String html_escape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c; break;
    }
  }
  return out;
}

inline String symbols_csv(const std::vector<String>& syms) {
  String out;
  for (size_t i = 0; i < syms.size(); ++i) {
    if (i) out += ',';
    out += syms[i];
  }
  return out;
}

inline String symbols_csv(const SettingsStore& settings) {
  return symbols_csv(settings.symbols());
}

template <typename Writer>
void write_settings_page(Writer& response, SettingsStore& settings) {
  response.print(PAGE_HEAD_P);
  response.print(F("<div class=topnav><h1>Settings</h1></div>"));

  if (settings.apiKey().length() == 0) {
    response.print(F("<div class=banner>No API token set.</div>"));
  }

  response.print(F("<form class=set method='POST' action='/settings'>"));
  response.print(F("<label>Refresh interval</label><input name=refresh_s type=number min='"));
  response.print(String(cfg::MIN_REFRESH_SECONDS));
  response.print(F("' value='"));
  response.print(String(settings.refreshSeconds()));
  response.print(F("' required>"
                   "<div class=hint>Seconds between quote refreshes.</div>"));

  response.print(F("<label>API token</label><input name=api_key value='"));
  response.print(html_escape(settings.apiKey()));
  response.print(F("' placeholder='paste your bearer token here'>"
                   "<div class=hint>Bearer token for "
                   "<code>rozakos.eu/stocks/api/v1</code>. Sent as "
                   "<code>Authorization: Bearer &lt;token&gt;</code>.</div>"
                   "<button type=submit>Save settings</button></form>"));

  response.print(F("<h2>Symbols</h2><div style='overflow-x:auto'>"
                   "<table><tr><th>Symbol</th><th></th></tr>"));
  auto syms = settings.symbols();
  if (syms.empty()) {
    response.print(F("<tr><td colspan=2 class=empty>No symbols yet &mdash; add one below.</td></tr>"));
  } else {
    for (size_t i = 0; i < syms.size(); ++i) {
      response.print(F("<tr><td><strong>"));
      response.print(html_escape(syms[i]));
      response.print(F("</strong></td><td><form class=inline method='POST' action='/delete'>"
                       "<input type=hidden name=i value='"));
      response.print(String((unsigned)i));
      response.print(F("'><button class=del type=submit>&times;</button></form></td></tr>"));
    }
  }
  response.print(F("</table></div>"));
  response.print(DATALIST_HTML);
  response.print(F("<h2>Add symbol</h2>"
                   "<form class=add method='POST' action='/add'>"
                   "<input name=symbol placeholder='AMD' maxlength=12 required list=symbols>"
                   "<button type=submit>Add</button></form>"
                   "<div class=hint>Heads up: device RAM limits brand logos &mdash; "
                   "beyond about 5 stocks, extra symbols show a lettered badge "
                   "instead of their icon.</div>"));
  response.print(PAGE_FOOT_P);
}

}  // namespace web_admin_page
