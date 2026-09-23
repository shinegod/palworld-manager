.PHONY: all clean build-frontend build dev

VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo "dev")
LDFLAGS := -s -w -X main.version=$(VERSION)

all: build

build-frontend:
	cd web && pnpm install && pnpm build

build: build-frontend
	go build -trimpath -ldflags "$(LDFLAGS)" -o bin/palmanager ./cmd/palmanager

build-linux: build-frontend
	CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags "$(LDFLAGS)" -o bin/palmanager-linux-amd64 ./cmd/palmanager

dev:
	go run ./cmd/palmanager -config configs/palmanager.example.yaml

clean:
	rm -rf bin/ web/dist/
