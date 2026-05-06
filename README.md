# NSU_DIS_SYS — distributed MD5 brute-forcer on Kubernetes

Fault-tolerant distributed system that finds the source string for a given
MD5 hash by brute force. Runs on Kubernetes; uses MongoDB (replica set) for
durable state and RabbitMQ for asynchronous task distribution.

## Repo layout

```
worker/   # C worker (brute force)
manager/  # C manager (API + orchestration)
k8s/      # Kubernetes manifests (kustomize)
```

## Local build

You need GCC, `libomp-dev`, `librabbitmq-dev`, `libmongoc-dev`, `libbson-dev`,
and `pkg-config`.

```sh
# worker
cd worker
gcc -O2 -o nob nob.c
./nob

# manager
cd ../manager
gcc -O2 -o nob nob.c
./nob
```

Binaries land in `./bin/`.

## Local Kubernetes deploy (kind)

```sh
# 1. Create a local cluster
kind create cluster --name crack

# 2. Build images and load them into the cluster
docker build -t crack/worker:latest  ./worker
docker build -t crack/manager:latest ./manager
kind load docker-image crack/worker:latest  --name crack
kind load docker-image crack/manager:latest --name crack

# 3. Apply manifests
kubectl apply -k k8s/

# 4. Wait for everything to come up
kubectl -n crack wait --for=condition=Ready pod -l app=mongodb --timeout=180s
kubectl -n crack wait --for=condition=Complete job/mongodb-init --timeout=180s
kubectl -n crack wait --for=condition=Ready pod -l app=rabbitmq --timeout=120s
kubectl -n crack wait --for=condition=Available deployment/manager --timeout=120s
kubectl -n crack rollout status deploy/worker --timeout=120s
```

Reach the API:

```sh
# Direct via NodePort (kind: port-forward to your host)
kubectl -n crack port-forward svc/manager 8080:8080
```
