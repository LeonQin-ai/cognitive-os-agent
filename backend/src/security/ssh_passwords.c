/* User-provided/local-vault SSH passwords, never inferred from entropy. */
#include "security/secret.h"
#include "infra/util.h"
#include "os/os_thread.h"
#include "cJSON.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct password_entry { char *value; size_t len; struct password_entry *next; } password_entry;
static password_entry *passwords;
static mutex_t password_mutex;
static _Atomic int ready;

static void password_lock(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&ready, &expected, 1)) {
        mutex_init(&password_mutex); atomic_store_explicit(&ready, 2, memory_order_release);
    } else while (atomic_load_explicit(&ready, memory_order_acquire) != 2) { }
    mutex_lock(&password_mutex);
}

static int register_value(const char *password) {
    size_t n = password ? strlen(password) : 0;
    if (!n || n > 6144) return -1;
    password_lock();
    for (password_entry *p = passwords; p; p = p->next)
        if (p->len == n && !memcmp(p->value, password, n)) { mutex_unlock(&password_mutex); return 0; }
    password_entry *p = calloc(1, sizeof(*p));
    if (p) p->value = xstrdup(password);
    if (!p || !p->value) { free(p); mutex_unlock(&password_mutex); return -1; }
    p->len = n; p->next = passwords; passwords = p;
    mutex_unlock(&password_mutex); return 0;
}

int secret_register_ssh_password(const char *password) {
    if (!password || !*password || strlen(password)>1024) return -1;
    if (register_value(password)) return -1;
    cJSON *value=cJSON_CreateString(password);
    char *json=value ? cJSON_PrintUnformatted(value):NULL; cJSON_Delete(value);
    if (!json) return -1;
    size_t n=strlen(json); json[n-1]=0;
    int rc=register_value(json+1);
    volatile char *wipe=json; for(size_t i=0;i<n;i++) wipe[i]=0;
    free(json); return rc;
}

size_t secret_ssh_password_max_length(void) {
    size_t n=0; password_lock();
    for(password_entry *p=passwords;p;p=p->next) if(p->len>n) n=p->len;
    mutex_unlock(&password_mutex); return n;
}

/* Short passwords must not erase every occurrence of a digit/article in code.
 * Prompt ingress already redacts their exact span; later echoes need a label. */
static int short_password_label(const char *text, size_t at) {
    size_t begin = at;
    while (begin && strchr(" \t:=\"'", text[begin-1])) begin--;
    const char *labels[] = {"password", "passwd", "pwd", "密码", "口令"};
    for (size_t i=0; i<sizeof(labels)/sizeof(labels[0]); i++) {
        size_t n=strlen(labels[i]);
        if (begin<n) continue;
        size_t k=0;
        for (; k<n; k++) if (tolower((unsigned char)text[begin-n+k]) != tolower((unsigned char)labels[i][k])) break;
        if (k==n) return 1;
    }
    return 0;
}

static int match_order(const void *a, const void *b) {
    const secret_match *x=a,*y=b;
    if (x->start!=y->start) return x->start<y->start ? -1:1;
    return x->end>y->end ? -1:x->end<y->end;
}

int secret_scan_ssh_passwords(const char *text, size_t len, secret_match **out) {
    secret_match *matches=NULL; size_t count=0, capacity=0;
    if (!out) return -1;
    *out=NULL; if (!text || !len) return 0;
    password_lock();
    for (password_entry *p=passwords; p; p=p->next) {
        if (p->len>len) continue;
        for (size_t at=0; at<=len-p->len; at++) {
            if (text[at]!=p->value[0] || memcmp(text+at,p->value,p->len) ||
                (p->len<4 && !short_password_label(text,at))) continue;
            if (count==capacity) {
                size_t next=capacity ? capacity*2:8;
                secret_match *grown=realloc(matches,next*sizeof(*matches));
                if (!grown) { free(matches); mutex_unlock(&password_mutex); return -1; }
                matches=grown; capacity=next;
            }
            secret_match *m=&matches[count++]; memset(m,0,sizeof(*m));
            m->start=at; m->end=at+p->len; m->severity=SECRET_SEV_HIGH;
            strcpy(m->kind,"ssh_password");
        }
    }
    mutex_unlock(&password_mutex);
    if (count>1) qsort(matches,count,sizeof(*matches),match_order);
    size_t kept=0;
    for(size_t i=0;i<count;i++) {
        if (kept && matches[i].start<matches[kept-1].end) {
            if (matches[i].end>matches[kept-1].end) matches[kept-1].end=matches[i].end;
        } else matches[kept++]=matches[i];
    }
    *out=matches; return (int)kept;
}

char *secret_redact_ssh_passwords(const char *text, size_t len, const char *replacement) {
    secret_match *matches=NULL;
    if (!text) return NULL;
    int count=secret_scan_ssh_passwords(text,len,&matches);
    if (count<0) return NULL;
    if (!count) { free(matches); return xstrdup(text); }
    strbuf out; strbuf_init(&out); size_t pos=0;
    for(int i=0;i<count;i++) {
        strbuf_append_n(&out,text+pos,matches[i].start-pos);
        strbuf_append(&out,replacement ? replacement:"[REDACTED:ssh_password]");
        pos=matches[i].end;
    }
    strbuf_append_n(&out,text+pos,len-pos); free(matches); return strbuf_detach(&out);
}
