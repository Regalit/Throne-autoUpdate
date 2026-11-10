package probe

import (
	"context"
	"net/http"
	"net/http/httptest"
	"reflect"
	"testing"
)

func checkServer(t *testing.T) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	mux.HandleFunc("/bulk", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(make([]byte, checkBulkBytes))
	})
	mux.HandleFunc("/short", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write(make([]byte, 16*1024))
	})
	mux.HandleFunc("/ok", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusNoContent) })
	mux.HandleFunc("/login", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusForbidden) })
	mux.HandleFunc("/refused", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte("<p>Gemini isn't currently supported in your country.</p>"))
	})
	mux.HandleFunc("/down", func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(http.StatusBadGateway) })
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv
}

func TestServerCheck(t *testing.T) {
	srv := checkServer(t)
	dead := httptest.NewServer(http.NotFoundHandler())
	dead.Close() // refuses connections

	cases := []struct {
		name     string
		bulk     []string
		services []checkService
		state    string
		failed   []string
	}{
		{"all pass", []string{srv.URL + "/bulk"},
			[]checkService{{Name: "A", URL: srv.URL + "/ok"}, {Name: "B", URL: srv.URL + "/login"}}, CheckOK, nil},
		{"second bulk target passes", []string{dead.URL + "/bulk", srv.URL + "/bulk"},
			[]checkService{{Name: "A", URL: srv.URL + "/ok"}}, CheckOK, nil},
		{"short volume is blocked", []string{srv.URL + "/short"},
			[]checkService{{Name: "A", URL: srv.URL + "/ok"}}, CheckBlocked, nil},
		{"no volume is blocked", []string{dead.URL + "/bulk"},
			[]checkService{{Name: "A", URL: srv.URL + "/ok"}}, CheckBlocked, nil},
		{"one service down", []string{srv.URL + "/bulk"},
			[]checkService{{Name: "A", URL: srv.URL + "/ok"}, {Name: "B", URL: srv.URL + "/down"}, {Name: "C", URL: dead.URL + "/"}}, CheckService, []string{"B", "C"}},
		{"refusal page is not the service", []string{srv.URL + "/bulk"},
			[]checkService{{Name: "A", URL: srv.URL + "/refused", Refused: []string{"supported in your country"}},
				{Name: "B", URL: srv.URL + "/refused"}}, CheckService, []string{"A"}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			got := runServerCheck(context.Background(), srv.Client(), tc.bulk, tc.services)
			if got.State != tc.state || !reflect.DeepEqual(got.Failed, tc.failed) {
				t.Fatalf("got %s %v, want %s %v", got.State, got.Failed, tc.state, tc.failed)
			}
			if tc.state != CheckBlocked && len(got.Services) != len(tc.services) {
				t.Fatalf("services %v", got.Services)
			}
			if tc.state != CheckBlocked && got.BulkBytes != checkBulkBytes {
				t.Fatalf("bulk bytes %d", got.BulkBytes)
			}
		})
	}
}
