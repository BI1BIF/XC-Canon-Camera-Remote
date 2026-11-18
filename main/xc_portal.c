#include "xc_portal.h"
#include <stdio.h>
#include <string.h>

xc_setup_mode_t xc_portal_lan_route(xc_setup_mode_t active, const char *uri)
{
    if (!strcmp(uri, "/camera")) return active == XC_SETUP_CAMERA ? active : XC_SETUP_HOME;
    if (!strcmp(uri, "/") || !strcmp(uri, "/display")) return XC_SETUP_DISPLAY;
    return XC_SETUP_HOME;
}

bool xc_portal_host_matches(const char *host, const char *ip)
{
    const size_t len = strlen(ip);
    return !strncmp(host, ip, len) && (!host[len] || !strcmp(host + len, ":80"));
}

bool xc_title_normalize(char *title)
{
    size_t len = strlen(title), left = strspn(title, " ");
    while (len > left && title[len - 1] == ' ') --len;
    len -= left;
    memmove(title, title + left, len);
    title[len] = 0;
    const unsigned char *s = (const unsigned char *)title;
    for (size_t i = 0, count = 0; i < len; ++count) {
        if (count == 12) return false;
        const unsigned c = s[i++];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == ' ' || c == '-' || c == '_' || c == '.') continue;
        if (c < 0xe4 || c > 0xe9 || len - i < 2 || (s[i] & 0xc0) != 0x80 || (s[i+1] & 0xc0) != 0x80)
            return false;
        const unsigned cp = ((c & 15) << 12) | ((s[i] & 63) << 6) | (s[i+1] & 63);
        if (cp < 0x4e00 || cp > 0x9fff) return false;
        i += 2;
    }
    return true;
}

static const char head[] =
    "<!doctype html><html lang=zh-CN><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<meta name=color-scheme content=dark><title>Passport XC · 本地设置</title>"
    "<style>"
    ":root{--bg:#131718;--surface:#202729;--fg:#f3f6f2;--muted:#afbfbd;--line:#3c4a4a;--accent:#8fe5d4}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,sans-serif}"
    "header{border-bottom:1px solid var(--line)}header div{min-height:62px;display:flex;gap:10px;align-items:center;justify-content:space-between}"
    ".wrap{width:min(100% - 40px,440px);margin-inline:auto}.brand{font-weight:740;letter-spacing:.025em}"
    ".local{font-size:13px;color:var(--muted);display:flex;align-items:center;gap:7px}.local:before{content:'';width:6px;height:6px;border-radius:50%;background:var(--accent)}"
    "main{padding:30px 0 max(28px,env(safe-area-inset-bottom))}.eyebrow{color:var(--accent);font-size:13px;font-weight:650;letter-spacing:.05em}"
    "h1{font-size:29px;line-height:1.25;letter-spacing:-.025em;margin:8px 0 9px}p{margin:0;color:var(--muted)}"
    "form{margin-top:25px}label{display:block;font-weight:650;margin:0 0 7px}.field+.field{margin-top:18px}"
    "input:not([type=hidden]),button{font:inherit;width:100%;min-height:48px;border-radius:7px;padding:10px 12px}"
    "input:not([type=hidden]){color:var(--fg);background:var(--surface);border:1px solid #5b6c6a}input::placeholder{color:var(--muted)}"
    "input:focus-visible,button:focus-visible,a:focus-visible{outline:2px solid var(--accent);outline-offset:3px}"
    ".hint{font-size:14px;margin-top:6px}button{margin-top:22px;color:var(--bg);background:var(--accent);border:1px solid var(--accent);font-weight:740;cursor:pointer}"
    "button:hover{background:#b2f4e7}button:active{background:#72c8b9}.foot{margin-top:25px;border-top:1px solid var(--line);padding-top:10px}"
    "a{color:var(--accent);display:inline-flex;align-items:center;min-height:44px;text-underline-offset:4px}"
    ".notice{margin-top:22px;color:#f7be73;border-left:2px solid currentColor;padding:4px 10px}"
    ".lcd{margin-top:22px;padding:12px 14px;border:1px solid #647876;border-radius:8px;background:#181e20;display:flex;align-items:center;justify-content:space-between;gap:12px}"
    ".lcd output{font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-weight:700}.lcd span{font-size:12px;color:var(--muted);white-space:nowrap}"
    "</style><header><div class=wrap><span class=brand>Passport XC</span><span class=local>设备本地设置</span></div></header>"
    "<main class=wrap>";

bool xc_portal_render(const xc_portal_t *p, bool (*send)(void *, const char *), void *ctx)
{
    #define SEND(s) do { if (!send(ctx, (s))) return false; } while (0)
    char line[512];
    const bool display = p->display || p->mode == XC_SETUP_DISPLAY;
    const bool camera = p->mode == XC_SETUP_CAMERA;
    SEND(head);
    if (p->mode == XC_SETUP_HOME) {
        SEND("<span class=eyebrow>局域网已连接</span><h1>Passport 设置</h1>"
             "<p>手机和 Passport 保持在同一个 Wi-Fi。要修改设置，请先在 Passport 上打开相应菜单。</p>"
             "<div class=foot><h2>相机地址与登录</h2><p>发现不到相机或相机要求登录时，选「手动连接相机」或「网页设置相机」。</p>"
             "<a href=/camera>打开相机设置</a></div>");
    } else if (p->submitted) {
        SEND(display ? "<span class=eyebrow>屏幕设置</span><h1>标题已保存</h1><p>新标题已显示在 Passport 屏幕左上角。重启后会保留。</p>"
             : camera ? "<span class=eyebrow>相机连接</span><h1>已提交相机信息</h1><p>请在 Passport 屏幕上查看连接结果。手机可以留在当前 Wi-Fi。</p>"
                      : "<span class=eyebrow>Wi-Fi 配网</span><h1>已提交网络信息</h1><p>请在 Passport 屏幕上查看连接结果。设置热点将关闭，手机可以切回原来的 Wi-Fi。</p>");
        if (display) SEND("<a href=/display>继续编辑标题</a>");
    } else {
        SEND(display ? "<span class=eyebrow>屏幕设置</span><h1>编辑屏幕标题</h1><p>设置 Passport 左上角显示的名称。</p>"
             : camera ? "<span class=eyebrow>相机连接</span><h1>手动连接相机</h1><p>填写佳能相机的本地地址。仅在相机要求登录时填写账号。</p>"
                      : "<span class=eyebrow>Wi-Fi 配网</span><h1>连接本地 Wi-Fi</h1><p>输入能访问相机的 2.4 GHz 网络。完成后请回到 Passport 操作。</p>");
        if (p->error) {
            SEND("<p class=notice role=alert>"); SEND(p->error); SEND("</p>");
        }
        if (display) {
            snprintf(line, sizeof(line), "<div class=lcd><output id=preview>%s</output><span>左上角预览</span></div>",
                     p->title[0] ? p->title : XC_TITLE_DEFAULT);
            SEND(line);
        }
        snprintf(line, sizeof(line), "<form method=post action='%s'><input type=hidden name=k value='%s'>",
                 display ? "/display" : camera && p->lan ? "/camera" : "/", p->nonce);
        SEND(line);
        if (display) {
            snprintf(line, sizeof(line), "<div class=field><label for=n>屏幕标题</label>"
                     "<input id=n name=n maxlength=12 value='%s' placeholder='" XC_TITLE_DEFAULT "' autocomplete=off aria-describedby=hint></div>",
                     p->title);
            SEND(line);
            SEND("<p class=hint id=hint>支持英文、数字、常用中文和空格、短横线、下划线或点。留空恢复默认。屏幕最多容纳 6 个汉字。</p>");
        } else if (camera) {
            snprintf(line, sizeof(line), "<div class=field><label for=i>相机 IPv4 地址</label>"
                     "<input id=i name=i inputmode=decimal required maxlength=15 value='%s' placeholder='如 192.168.1.24'></div>"
                     "<div class=field><label for=t>XC 端口</label><input id=t name=t type=number min=1 max=65535 required value=%u></div>",
                     p->ip, p->port);
            SEND(line);
            SEND("<div class=field><label for=u>XC 用户名</label><input id=u name=u maxlength=48 autocomplete=off></div>"
                 "<div class=field><label for=p>XC 密码</label><input id=p name=p type=password maxlength=64 autocomplete=off></div>");
        }
        if (p->lan && camera) SEND("<div class=field><label for=a>设备屏幕上的验证码</label>"
                         "<input id=a name=a type=password required minlength=8 maxlength=8 autocomplete=off autocapitalize=none spellcheck=false></div>");
        if (display) {
            SEND("<button>保存标题</button></form><script>const n=document.getElementById('n'),o=document.getElementById('preview');"
                 "n.addEventListener('input',()=>o.textContent=n.value.trim()||'" XC_TITLE_DEFAULT "');</script>");
        } else if (camera) {
            SEND("<button>连接相机</button></form>");
        } else {
            SEND("<div class=field><label for=s>Wi-Fi 名称（SSID）</label>"
                 "<input id=s name=s required maxlength=32 autocomplete=off autocapitalize=none spellcheck=false placeholder='如 Studio-2G'></div>"
                 "<div class=field><label for=p>Wi-Fi 密码</label>"
                 "<input id=p name=p type=password maxlength=64 autocomplete=off aria-describedby=hint>"
                 "<p class=hint id=hint>开放网络可以留空。</p></div><button>连接 Wi-Fi</button></form>");
        }
    }
    if (p->lan && display) SEND("<div class=foot><a href=/camera>相机地址与登录</a></div>");
    else if (p->lan && !display) SEND("<div class=foot><a href=/>编辑屏幕标题</a></div>");
    else if (display && p->mode != XC_SETUP_DISPLAY) SEND("<div class=foot><a href=/>返回连接设置</a></div>");
    else if (!p->lan && p->mode == XC_SETUP_WIFI && !p->submitted) SEND("<div class=foot><a href=/display>编辑屏幕标题</a></div>");
    SEND("</main></html>");
    #undef SEND
    return true;
}
