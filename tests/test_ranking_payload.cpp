// Test del contrato puro RankingPayload.h (compila en Linux, sin Winsock).
#include "../src/output/RankingPayload.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

static int fallos = 0;

static void check(const std::string& nombre, bool ok, const std::string& detalle = "") {
    std::cout << (ok ? "[OK]   " : "[FAIL] ") << nombre;
    if (!detalle.empty()) std::cout << "  -> " << detalle;
    std::cout << "\n";
    if (!ok) fallos++;
}

int main() {
    // Caso 1: ejemplo exacto del dueno (10 canicas, orden completo)
    {
        ranking::RankingState st;
        // orden representado por el JSON: [8,2,5,1,4,9,10,6,7,3]
        std::vector<int> orden = {8, 2, 5, 1, 4, 9, 10, 6, 7, 3};
        bool cambio = st.Update(orden);
        check("primer Update() reporta cambio", cambio == true);
        st.seq = 201;
        std::string p = st.BuildPayload();
        std::string esperado =
            "{\"type\":\"ranking\",\"seq\":201,\"canicas\":["
            "{\"id\":0,\"puesto\":7},{\"id\":1,\"puesto\":4},{\"id\":2,\"puesto\":2},"
            "{\"id\":3,\"puesto\":10},{\"id\":4,\"puesto\":5},{\"id\":5,\"puesto\":3},"
            "{\"id\":6,\"puesto\":8},{\"id\":7,\"puesto\":9},{\"id\":8,\"puesto\":1},"
            "{\"id\":9,\"puesto\":6}]}";
        std::cout << "  payload: " << p << "\n  esperado: " << esperado << "\n";
        check("payload coincide con el contrato TD", p == esperado);
        check("bytes < 1200 (cabe en un datagrama)", p.size() < 1200,
              std::to_string(p.size()) + " bytes");
    }

    // Caso 2: mismo orden -> no cambia (no se emite)
    {
        ranking::RankingState st;
        std::vector<int> orden = {8, 2, 5, 1, 4, 9, 10, 6, 7, 3};
        st.Update(orden);
        bool segundo = st.Update(orden);
        check("mismo orden NO reporta cambio", segundo == false);
    }

    // Caso 3: canica no detectada -> queda en puesto 0 (indeterminado)
    {
        ranking::RankingState st;
        std::vector<int> completo = {8, 2, 5, 1, 4, 9, 10, 6, 7, 3};
        st.Update(completo);
        // faltan la 3 y la 7
        std::vector<int> parcial = {8, 2, 5, 1, 4, 9, 10, 6};
        st.Update(parcial);
        check("canica 3 no detectada -> puesto 0", st.lastPuesto[3] == 0,
              "puesto=" + std::to_string(st.lastPuesto[3]));
        check("canica 7 no detectada -> puesto 0", st.lastPuesto[7] == 0,
              "puesto=" + std::to_string(st.lastPuesto[7]));
        std::string payload = st.BuildPayload();
        check("array sigue teniendo 10 entradas",
              std::count(payload.begin(), payload.end(), '{') == 11);
    }

    // Caso 4: mapeo de ids
    {
        check("MapBallIdToTd(1) == 10 (bola_0 es la canica 10)", ranking::MapBallIdToTd(1) == 10);
        check("MapBallIdToTd(2) == 1", ranking::MapBallIdToTd(2) == 1);
        check("MapBallIdToTd(10) == 9", ranking::MapBallIdToTd(10) == 9);
    }

    // Caso 5: una emision por punto de medicion (6 MeasurePoint de la coreografia)
    {
        ranking::RankingState st;
        std::vector<std::vector<int>> puntos = {
            {8, 2, 5, 1, 4, 9, 10, 6, 7, 3},
            {2, 8, 5, 4, 1, 10, 9, 6, 7, 3},
            {2, 5, 8, 4, 1, 10, 6, 9, 7, 3},
            {2, 5, 4, 8, 1, 6, 10, 9, 7, 3},
            {2, 5, 4, 1, 8, 6, 9, 10, 7, 3},
            {2, 5, 4, 1, 8, 6, 7, 9, 10, 3},
        };
        bool ok = true;
        for (size_t i = 0; i < puntos.size(); ++i) {
            st.Update(puntos[i]);
            st.seq += 1;              // lo hace el publisher, una vez por punto
            if (st.seq != i + 1) ok = false;
        }
        check("6 puntos -> seq 1..6 (una emision por punto)", ok,
              "seq final=" + std::to_string(st.seq));
    }

    std::cout << "\n" << (fallos == 0 ? "TODO OK" : std::to_string(fallos) + " FALLOS") << "\n";
    return fallos == 0 ? 0 : 1;
}
