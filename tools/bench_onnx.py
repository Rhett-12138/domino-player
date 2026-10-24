#!/usr/bin/env python3
"""Benchmark an ONNX SR model with ONNX Runtime execution providers.

Install the runtime for the selected provider first, e.g.:
  pip install onnxruntime-gpu        # CUDA / TensorRT EP (needs CUDA/cuDNN/TensorRT)
  pip install onnxruntime-directml   # DirectML EP (Windows)
"""
import argparse
import time

import numpy as np
import onnxruntime as ort


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--model', required=True)
    p.add_argument('--provider', default='cpu',
                   choices=['cpu', 'cuda', 'tensorrt', 'dml'])
    p.add_argument('--width', type=int, default=960)
    p.add_argument('--height', type=int, default=540)
    p.add_argument('--batch', type=int, default=1)
    p.add_argument('--loops', type=int, default=20)
    p.add_argument('--cache', default='')
    args = p.parse_args()

    provider_map = {
        'cpu': 'CPUExecutionProvider',
        'cuda': 'CUDAExecutionProvider',
        'tensorrt': 'TensorrtExecutionProvider',
        'dml': 'DmlExecutionProvider',
    }
    provider = provider_map[args.provider]
    available = ort.get_available_providers()
    print('available:', available)
    if provider not in available:
        raise SystemExit(f'{provider} not available; install the matching onnxruntime package')

    so = ort.SessionOptions()
    providers = [provider]
    if args.provider == 'tensorrt':
        opts = {
            'trt_fp16_enable': '1',
            'trt_engine_cache_enable': '1',
        }
        if args.cache:
            opts['trt_engine_cache_path'] = args.cache
        providers = [(provider, opts), 'CUDAExecutionProvider']
    elif args.provider == 'cuda':
        providers = [provider]

    sess = ort.InferenceSession(args.model, sess_options=so, providers=providers)
    inp = sess.get_inputs()[0]
    out = sess.get_outputs()[0]
    print('input:', inp.name, inp.shape, inp.type)
    print('output:', out.name, out.shape, out.type)
    x = np.random.rand(args.batch, 3, args.height, args.width).astype(np.float32)
    feed = {inp.name: x}

    y = sess.run(None, feed)[0]
    for _ in range(2):
        sess.run(None, feed)
    t0 = time.perf_counter()
    for _ in range(args.loops):
        y = sess.run(None, feed)[0]
    dt = (time.perf_counter() - t0) / args.loops
    print(f'provider={provider} batch={args.batch} '
          f'in={args.width}x{args.height} out={y.shape} '
          f'ms={dt * 1000:.2f} fps={1.0 / dt:.2f}')


if __name__ == '__main__':
    main()
