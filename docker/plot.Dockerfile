FROM python:3.11-slim

RUN pip install --no-cache-dir \
      pandas==2.2.2 \
      matplotlib==3.9.2 \
      seaborn==0.13.2

WORKDIR /work

ENTRYPOINT ["python3", "/work/scripts/plot_executors.py"]
CMD ["--results-dir", "/data/results"]
