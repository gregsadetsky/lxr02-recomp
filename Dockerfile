FROM python:3.12-slim
COPY --from=ghcr.io/astral-sh/uv:0.5.11 /uv /bin/uv
WORKDIR /code
COPY pyproject.toml uv.lock /code/
RUN uv sync --frozen --no-dev
COPY app.py /code/
COPY 4.web/site /code/4.web/site
EXPOSE 8000
CMD ["uv", "run", "--frozen", "gunicorn", "-b", "0.0.0.0:8000", "app:app"]
