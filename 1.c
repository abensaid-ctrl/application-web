/*
 * =====================================================
 *   BIBLIOTHEQUE - Serveur HTTP en C avec SQLite
 *   Compilation: gcc server.c -o server -lsqlite3 -lpthread
 *   Lancement:   ./server 8080
 * =====================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sqlite3.h>
#include <time.h>
#include <ctype.h>

#define PORT 8080
#define BUFFER_SIZE 65536
#define MAX_RESPONSE 262144

/* ─────────────── DATABASE ─────────────── */

static sqlite3 *db = NULL;

void db_init(void) {
    int rc = sqlite3_open("bibliotheque.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        exit(1);
    }

    const char *sql =
        "PRAGMA foreign_keys = ON;"

        "CREATE TABLE IF NOT EXISTS membres ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  nom TEXT NOT NULL,"
        "  prenom TEXT NOT NULL,"
        "  email TEXT UNIQUE NOT NULL,"
        "  telephone TEXT,"
        "  adresse TEXT,"
        "  date_inscription TEXT DEFAULT (date('now')),"
        "  actif INTEGER DEFAULT 1"
        ");"

        "CREATE TABLE IF NOT EXISTS livres ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  titre TEXT NOT NULL,"
        "  auteur TEXT NOT NULL,"
        "  isbn TEXT UNIQUE,"
        "  editeur TEXT,"
        "  annee INTEGER,"
        "  categorie TEXT,"
        "  exemplaires INTEGER DEFAULT 1,"
        "  disponibles INTEGER DEFAULT 1,"
        "  description TEXT"
        ");"

        "CREATE TABLE IF NOT EXISTS emprunts ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  membre_id INTEGER NOT NULL,"
        "  livre_id INTEGER NOT NULL,"
        "  date_emprunt TEXT DEFAULT (date('now')),"
        "  date_retour_prevue TEXT NOT NULL,"
        "  date_retour_reelle TEXT,"
        "  statut TEXT DEFAULT 'en_cours',"
        "  FOREIGN KEY (membre_id) REFERENCES membres(id),"
        "  FOREIGN KEY (livre_id) REFERENCES livres(id)"
        ");";

    char *errmsg = NULL;
    rc = sqlite3_exec(db, sql, 0, 0, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", errmsg);
        sqlite3_free(errmsg);
    }

    /* Données de démo */
    sqlite3_exec(db,
        "INSERT OR IGNORE INTO membres (nom, prenom, email, telephone) VALUES "
        "('Martin', 'Sophie', 'sophie@email.fr', '0612345678'),"
        "('Durand', 'Pierre', 'pierre@email.fr', '0698765432'),"
        "('Bernard', 'Marie', 'marie@email.fr', '0654321987');"

        "INSERT OR IGNORE INTO livres (titre, auteur, isbn, editeur, annee, categorie, exemplaires, disponibles) VALUES "
        "('Le Petit Prince', 'Antoine de Saint-Exupéry', '9782070408504', 'Gallimard', 1943, 'Littérature', 3, 3),"
        "('1984', 'George Orwell', '9782072795763', 'Gallimard', 1949, 'Roman', 2, 2),"
        "('L Étranger', 'Albert Camus', '9782070360024', 'Gallimard', 1942, 'Littérature', 2, 2),"
        "('Harry Potter T1', 'J.K. Rowling', '9782070584628', 'Gallimard Jeunesse', 1998, 'Fantasy', 4, 4),"
        "('Dune', 'Frank Herbert', '9782221252055', 'Robert Laffont', 1965, 'Science-Fiction', 2, 2);",
    0, 0, NULL);

    printf("[DB] Base de données initialisée.\n");
}

/* ─────────────── JSON HELPERS ─────────────── */

char *json_escape(const char *s) {
    if (!s) return strdup("\"\"");
    size_t len = strlen(s);
    char *out = malloc(len * 6 + 3);
    int j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')       { out[j++]='\\'; out[j++]='"'; }
        else if (c == '\\') { out[j++]='\\'; out[j++]='\\'; }
        else if (c == '\n') { out[j++]='\\'; out[j++]='n'; }
        else if (c == '\r') { out[j++]='\\'; out[j++]='r'; }
        else if (c == '\t') { out[j++]='\\'; out[j++]='t'; }
        else                { out[j++] = c; }
    }
    out[j++] = '"';
    out[j]   = '\0';
    return out;
}

/* ─────────────── URL DECODE ─────────────── */

static int hex_val(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return 0;
}

void url_decode(char *dst, const char *src, size_t max) {
    size_t i=0, j=0;
    while (src[i] && j<max-1) {
        if (src[i]=='%' && src[i+1] && src[i+2]) {
            dst[j++] = (char)(hex_val(src[i+1])*16 + hex_val(src[i+2]));
            i+=3;
        } else if (src[i]=='+') {
            dst[j++]=' '; i++;
        } else {
            dst[j++]=src[i++];
        }
    }
    dst[j]='\0';
}

/* ─────────────── PARAM PARSER ─────────────── */

char *get_param(const char *body, const char *key) {
    static char val[1024];
    char search[128];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) return NULL;
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end-p) : strlen(p);
    if (len > 900) len = 900;
    char tmp[1024];
    memcpy(tmp, p, len);
    tmp[len] = '\0';
    url_decode(val, tmp, sizeof(val));
    return val;
}

/* ─────────────── HTTP RESPONSE ─────────────── */

void send_response(int sock, int code, const char *content_type,
                   const char *body, size_t body_len) {
    char header[512];
    const char *status = (code==200)?"OK":(code==201)?"Created":
                         (code==400)?"Bad Request":(code==404)?"Not Found":"Internal Server Error";
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n\r\n",
        code, status, content_type, body_len);
    send(sock, header, hlen, 0);
    if (body && body_len > 0)
        send(sock, body, body_len, 0);
}

void send_json(int sock, int code, const char *json) {
    send_response(sock, code, "application/json", json, strlen(json));
}

void send_error(int sock, int code, const char *msg) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    send_json(sock, code, buf);
}

/* ─────────────── API HANDLERS ─────────────── */

/* GET /api/stats */
void api_stats(int sock) {
    sqlite3_stmt *stmt;
    char json[512];
    int membres=0, livres=0, emprunts=0, retards=0;

    sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM membres WHERE actif=1",-1,&stmt,NULL);
    if(sqlite3_step(stmt)==SQLITE_ROW) membres=sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM livres",-1,&stmt,NULL);
    if(sqlite3_step(stmt)==SQLITE_ROW) livres=sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM emprunts WHERE statut='en_cours'",-1,&stmt,NULL);
    if(sqlite3_step(stmt)==SQLITE_ROW) emprunts=sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM emprunts WHERE statut='en_cours' AND date_retour_prevue < date('now')",-1,&stmt,NULL);
    if(sqlite3_step(stmt)==SQLITE_ROW) retards=sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);

    snprintf(json, sizeof(json),
        "{\"membres\":%d,\"livres\":%d,\"emprunts\":%d,\"retards\":%d}",
        membres, livres, emprunts, retards);
    send_json(sock, 200, json);
}

/* GET /api/membres */
void api_get_membres(int sock) {
    sqlite3_stmt *stmt;
    char *json = malloc(MAX_RESPONSE);
    int pos = 0;
    pos += sprintf(json + pos, "[");
    int first = 1;

    sqlite3_prepare_v2(db,
        "SELECT id,nom,prenom,email,telephone,adresse,date_inscription,actif FROM membres ORDER BY nom,prenom",
        -1, &stmt, NULL);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) pos += sprintf(json+pos, ",");
        first = 0;
        char *id   = (char*)sqlite3_column_text(stmt,0);
        char *nom  = json_escape((char*)sqlite3_column_text(stmt,1));
        char *prenom=json_escape((char*)sqlite3_column_text(stmt,2));
        char *email= json_escape((char*)sqlite3_column_text(stmt,3));
        char *tel  = json_escape((char*)sqlite3_column_text(stmt,4));
        char *adr  = json_escape((char*)sqlite3_column_text(stmt,5));
        char *date = json_escape((char*)sqlite3_column_text(stmt,6));
        int  actif = sqlite3_column_int(stmt,7);

        pos += sprintf(json+pos,
            "{\"id\":%s,\"nom\":%s,\"prenom\":%s,\"email\":%s,"
            "\"telephone\":%s,\"adresse\":%s,\"date_inscription\":%s,\"actif\":%d}",
            id, nom, prenom, email, tel, adr, date, actif);
        free(nom); free(prenom); free(email); free(tel); free(adr); free(date);
    }
    sqlite3_finalize(stmt);
    pos += sprintf(json+pos, "]");
    send_json(sock, 200, json);
    free(json);
}

/* POST /api/membres */
void api_add_membre(int sock, const char *body) {
    char *nom    = get_param(body, "nom");
    char *prenom = get_param(body, "prenom");
    char *email  = get_param(body, "email");
    char *tel    = get_param(body, "telephone");
    char *adr    = get_param(body, "adresse");

    if (!nom || !prenom || !email) {
        send_error(sock, 400, "Champs obligatoires manquants");
        return;
    }

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "INSERT INTO membres (nom,prenom,email,telephone,adresse) VALUES (?,?,?,?,?)",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt,1,nom,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,prenom,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,3,email,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,4,tel?tel:"",-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,5,adr?adr:"",-1,SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        long long id = sqlite3_last_insert_rowid(db);
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"success\":true,\"id\":%lld}", id);
        send_json(sock, 201, resp);
    } else {
        send_error(sock, 500, "Email deja utilise");
    }
}

/* DELETE /api/membres/:id */
void api_del_membre(int sock, int id) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "UPDATE membres SET actif=0 WHERE id=?", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    send_json(sock, 200, "{\"success\":true}");
}

/* GET /api/livres */
void api_get_livres(int sock) {
    sqlite3_stmt *stmt;
    char *json = malloc(MAX_RESPONSE);
    int pos = 0;
    pos += sprintf(json+pos, "[");
    int first = 1;

    sqlite3_prepare_v2(db,
        "SELECT id,titre,auteur,isbn,editeur,annee,categorie,exemplaires,disponibles,description FROM livres ORDER BY titre",
        -1, &stmt, NULL);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) pos += sprintf(json+pos, ",");
        first = 0;
        char *id   = (char*)sqlite3_column_text(stmt,0);
        char *tit  = json_escape((char*)sqlite3_column_text(stmt,1));
        char *aut  = json_escape((char*)sqlite3_column_text(stmt,2));
        char *isbn = json_escape((char*)sqlite3_column_text(stmt,3));
        char *edit = json_escape((char*)sqlite3_column_text(stmt,4));
        char *ann  = (char*)sqlite3_column_text(stmt,5);
        char *cat  = json_escape((char*)sqlite3_column_text(stmt,6));
        int  expl  = sqlite3_column_int(stmt,7);
        int  dispo = sqlite3_column_int(stmt,8);
        char *desc = json_escape((char*)sqlite3_column_text(stmt,9));

        pos += sprintf(json+pos,
            "{\"id\":%s,\"titre\":%s,\"auteur\":%s,\"isbn\":%s,\"editeur\":%s,"
            "\"annee\":%s,\"categorie\":%s,\"exemplaires\":%d,\"disponibles\":%d,\"description\":%s}",
            id, tit, aut, isbn, edit, ann?ann:"0", cat, expl, dispo, desc);
        free(tit); free(aut); free(isbn); free(edit); free(cat); free(desc);
    }
    sqlite3_finalize(stmt);
    pos += sprintf(json+pos, "]");
    send_json(sock, 200, json);
    free(json);
}

/* POST /api/livres */
void api_add_livre(int sock, const char *body) {
    char *titre  = get_param(body,"titre");
    char *auteur = get_param(body,"auteur");
    char *isbn   = get_param(body,"isbn");
    char *edit   = get_param(body,"editeur");
    char *ann    = get_param(body,"annee");
    char *cat    = get_param(body,"categorie");
    char *expl_s = get_param(body,"exemplaires");
    char *desc   = get_param(body,"description");

    if (!titre || !auteur) {
        send_error(sock, 400, "Titre et auteur obligatoires");
        return;
    }
    int expl = expl_s ? atoi(expl_s) : 1;
    int annee = ann ? atoi(ann) : 0;

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "INSERT INTO livres (titre,auteur,isbn,editeur,annee,categorie,exemplaires,disponibles,description) VALUES (?,?,?,?,?,?,?,?,?)",
        -1, &stmt, NULL);
    sqlite3_bind_text(stmt,1,titre,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,auteur,-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,3,isbn?isbn:"",-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,4,edit?edit:"",-1,SQLITE_STATIC);
    sqlite3_bind_int(stmt,5,annee);
    sqlite3_bind_text(stmt,6,cat?cat:"",-1,SQLITE_STATIC);
    sqlite3_bind_int(stmt,7,expl);
    sqlite3_bind_int(stmt,8,expl);
    sqlite3_bind_text(stmt,9,desc?desc:"",-1,SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        long long id = sqlite3_last_insert_rowid(db);
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"success\":true,\"id\":%lld}", id);
        send_json(sock, 201, resp);
    } else {
        send_error(sock, 500, "Erreur insertion livre");
    }
}

/* DELETE /api/livres/:id */
void api_del_livre(int sock, int id) {
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "DELETE FROM livres WHERE id=?", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    send_json(sock, 200, "{\"success\":true}");
}

/* GET /api/emprunts */
void api_get_emprunts(int sock) {
    sqlite3_stmt *stmt;
    char *json = malloc(MAX_RESPONSE);
    int pos = 0;
    pos += sprintf(json+pos, "[");
    int first = 1;

    sqlite3_prepare_v2(db,
        "SELECT e.id, m.nom||' '||m.prenom, l.titre, e.date_emprunt, "
        "e.date_retour_prevue, e.date_retour_reelle, e.statut, e.membre_id, e.livre_id "
        "FROM emprunts e "
        "JOIN membres m ON m.id=e.membre_id "
        "JOIN livres l ON l.id=e.livre_id "
        "ORDER BY e.date_emprunt DESC",
        -1, &stmt, NULL);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) pos += sprintf(json+pos, ",");
        first = 0;
        char *id    = (char*)sqlite3_column_text(stmt,0);
        char *mem   = json_escape((char*)sqlite3_column_text(stmt,1));
        char *liv   = json_escape((char*)sqlite3_column_text(stmt,2));
        char *de    = json_escape((char*)sqlite3_column_text(stmt,3));
        char *drp   = json_escape((char*)sqlite3_column_text(stmt,4));
        char *drr   = json_escape((char*)sqlite3_column_text(stmt,5));
        char *stat  = json_escape((char*)sqlite3_column_text(stmt,6));
        char *mid   = (char*)sqlite3_column_text(stmt,7);
        char *lid   = (char*)sqlite3_column_text(stmt,8);

        pos += sprintf(json+pos,
            "{\"id\":%s,\"membre\":%s,\"livre\":%s,\"date_emprunt\":%s,"
            "\"date_retour_prevue\":%s,\"date_retour_reelle\":%s,\"statut\":%s,"
            "\"membre_id\":%s,\"livre_id\":%s}",
            id, mem, liv, de, drp, drr, stat, mid?mid:"0", lid?lid:"0");
        free(mem); free(liv); free(de); free(drp); free(drr); free(stat);
    }
    sqlite3_finalize(stmt);
    pos += sprintf(json+pos, "]");
    send_json(sock, 200, json);
    free(json);
}

/* POST /api/emprunts */
void api_add_emprunt(int sock, const char *body) {
    char *mid_s = get_param(body,"membre_id");
    char *lid_s = get_param(body,"livre_id");
    char *drp   = get_param(body,"date_retour_prevue");

    if (!mid_s || !lid_s || !drp) {
        send_error(sock, 400, "Champs manquants");
        return;
    }
    int mid = atoi(mid_s), lid = atoi(lid_s);

    /* Vérifier disponibilité */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT disponibles FROM livres WHERE id=?", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, lid);
    int dispo = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) dispo = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (dispo <= 0) {
        send_error(sock, 400, "Livre non disponible");
        return;
    }

    /* Créer emprunt */
    sqlite3_prepare_v2(db,
        "INSERT INTO emprunts (membre_id,livre_id,date_retour_prevue) VALUES (?,?,?)",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt,1,mid);
    sqlite3_bind_int(stmt,2,lid);
    sqlite3_bind_text(stmt,3,drp,-1,SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        sqlite3_prepare_v2(db,
            "UPDATE livres SET disponibles=disponibles-1 WHERE id=?",
            -1, &stmt, NULL);
        sqlite3_bind_int(stmt,1,lid);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        send_json(sock, 201, "{\"success\":true}");
    } else {
        send_error(sock, 500, "Erreur emprunt");
    }
}

/* POST /api/emprunts/:id/retour */
void api_retour(int sock, int id) {
    sqlite3_stmt *stmt;

    /* Récupérer livre_id */
    sqlite3_prepare_v2(db, "SELECT livre_id FROM emprunts WHERE id=? AND statut='en_cours'", -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, id);
    int lid = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) lid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (!lid) { send_error(sock, 404, "Emprunt introuvable"); return; }

    sqlite3_prepare_v2(db,
        "UPDATE emprunts SET statut='rendu', date_retour_reelle=date('now') WHERE id=?",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db,
        "UPDATE livres SET disponibles=disponibles+1 WHERE id=?",
        -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, lid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    send_json(sock, 200, "{\"success\":true}");
}

/* ─────────────── REQUEST HANDLER ─────────────── */

typedef struct {
    char method[8];
    char path[256];
    char body[BUFFER_SIZE];
    int  body_len;
} HttpRequest;

int parse_request(const char *raw, HttpRequest *req) {
    memset(req, 0, sizeof(*req));
    sscanf(raw, "%7s %255s", req->method, req->path);

    /* Body après \r\n\r\n */
    const char *b = strstr(raw, "\r\n\r\n");
    if (b) {
        b += 4;
        req->body_len = strlen(b);
        if (req->body_len >= BUFFER_SIZE) req->body_len = BUFFER_SIZE-1;
        memcpy(req->body, b, req->body_len);
        req->body[req->body_len] = '\0';
    }
    return 1;
}

void handle_client(int sock) {
    char *raw = malloc(BUFFER_SIZE);
    int n = recv(sock, raw, BUFFER_SIZE-1, 0);
    if (n <= 0) { free(raw); close(sock); return; }
    raw[n] = '\0';

    HttpRequest req;
    parse_request(raw, &req);
    free(raw);

    printf("[%s] %s\n", req.method, req.path);

    /* OPTIONS preflight */
    if (strcmp(req.method, "OPTIONS") == 0) {
        send_response(sock, 200, "text/plain", "", 0);
        close(sock); return;
    }

    /* Routing */
    if (strcmp(req.path, "/api/stats") == 0 && strcmp(req.method,"GET")==0) {
        api_stats(sock);
    }
    else if (strcmp(req.path, "/api/membres") == 0 && strcmp(req.method,"GET")==0) {
        api_get_membres(sock);
    }
    else if (strcmp(req.path, "/api/membres") == 0 && strcmp(req.method,"POST")==0) {
        api_add_membre(sock, req.body);
    }
    else if (strncmp(req.path, "/api/membres/", 13)==0 && strcmp(req.method,"DELETE")==0) {
        int id = atoi(req.path+13);
        api_del_membre(sock, id);
    }
    else if (strcmp(req.path, "/api/livres") == 0 && strcmp(req.method,"GET")==0) {
        api_get_livres(sock);
    }
    else if (strcmp(req.path, "/api/livres") == 0 && strcmp(req.method,"POST")==0) {
        api_add_livre(sock, req.body);
    }
    else if (strncmp(req.path, "/api/livres/", 12)==0 && strcmp(req.method,"DELETE")==0) {
        int id = atoi(req.path+12);
        api_del_livre(sock, id);
    }
    else if (strcmp(req.path, "/api/emprunts") == 0 && strcmp(req.method,"GET")==0) {
        api_get_emprunts(sock);
    }
    else if (strcmp(req.path, "/api/emprunts") == 0 && strcmp(req.method,"POST")==0) {
        api_add_emprunt(sock, req.body);
    }
    else if (strncmp(req.path, "/api/emprunts/", 14)==0 && strcmp(req.method,"POST")==0) {
        int id = atoi(req.path+14);
        api_retour(sock, id);
    }
    else {
        send_error(sock, 404, "Route non trouvee");
    }

    close(sock);
}

/* ─────────────── THREAD WRAPPER ─────────────── */

void *thread_fn(void *arg) {
    int sock = *(int*)arg;
    free(arg);
    handle_client(sock);
    return NULL;
}

/* ─────────────── MAIN ─────────────── */

int main(int argc, char *argv[]) {
    int port = (argc > 1) ? atoi(argv[1]) : PORT;

    db_init();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(server_fd, 16) < 0) { perror("listen"); exit(1); }

    printf("\n╔══════════════════════════════════════╗\n");
    printf("║  📚 Serveur Bibliothèque en C          ║\n");
    printf("║  http://localhost:%d                  ║\n", port);
    printf("╚══════════════════════════════════════╝\n\n");

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int *csock = malloc(sizeof(int));
        *csock = accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (*csock < 0) { free(csock); continue; }

        pthread_t tid;
        pthread_create(&tid, NULL, thread_fn, csock);
        pthread_detach(tid);
    }

    sqlite3_close(db);
    close(server_fd);
    return 0;
}
